#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "surface_lifecycle.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = -1; /* Exclude this directory descriptor. */
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    closedir(directory);
    return count;
}
struct gate {
    struct wl_display *display;
    struct wl_compositor *compositor;
    VkInstance instance;
    PFN_vkCreateWaylandSurfaceKHR create;
    PFN_vkDestroySurfaceKHR destroy;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_barrier_t live;
    int start, cancel;
};
struct worker { struct gate *gate; unsigned completed; };
static void *run(void *opaque) {
    struct worker *worker = opaque;
    struct gate *g = worker->gate;
    pthread_mutex_lock(&g->mutex);
    while (!g->start) pthread_cond_wait(&g->condition, &g->mutex);
    int cancel = g->cancel;
    pthread_mutex_unlock(&g->mutex);
    if (cancel) return NULL;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        struct wl_surface *windows[4] = {0};
        VkSurfaceKHR surfaces[4] = {0};
        unsigned created = 0;
        for (; created < 4; ++created) {
            windows[created] = wl_compositor_create_surface(g->compositor);
            if (!windows[created]) break;
            VkWaylandSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                .display = g->display, .surface = windows[created]};
            VkResult result = g->create(g->instance, &info, NULL, &surfaces[created]);
            if (result != VK_SUCCESS) {
                printf("WSI_LIFECYCLE create=%d cycle=%u\n", result, cycle);
                break;
            }
        }
        /* Every started worker arrives even if one failed to create a surface. */
        pthread_barrier_wait(&g->live);
        for (unsigned i = 4; i-- > 0;) {
            if (surfaces[i]) g->destroy(g->instance, surfaces[i], NULL);
            if (windows[i]) wl_surface_destroy(windows[i]);
        }
        pthread_barrier_wait(&g->live);
        if (created == 4) ++worker->completed;
    }
    return NULL;
}
int surface_lifecycle(struct wl_display *display, struct wl_compositor *compositor,
                      VkInstance instance, PFN_vkCreateWaylandSurfaceKHR create,
                      PFN_vkDestroySurfaceKHR destroy) {
    /* Warm backend one-time initialization before checking steady-state FDs. */
    struct wl_surface *window = wl_compositor_create_surface(compositor);
    if (!window) return 2;
    VkWaylandSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = display, .surface = window};
    VkSurfaceKHR surface;
    VkResult result = create(instance, &info, NULL, &surface);
    if (result != VK_SUCCESS) return 2;
    destroy(instance, surface, NULL);
    wl_surface_destroy(window);
    if (wl_display_roundtrip(display) < 0) return 2;
    int before = fd_count();
    struct gate gate = {.display = display, .compositor = compositor, .instance = instance,
        .create = create, .destroy = destroy, .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER};
    if (pthread_barrier_init(&gate.live, NULL, 4)) return 2;
    struct worker workers[4] = {0};
    pthread_t threads[4];
    unsigned started = 0;
    for (; started < 4; ++started) {
        workers[started].gate = &gate;
        if (pthread_create(&threads[started], NULL, run, &workers[started])) break;
    }
    pthread_mutex_lock(&gate.mutex);
    gate.cancel = started != 4;
    gate.start = 1;
    pthread_cond_broadcast(&gate.condition);
    pthread_mutex_unlock(&gate.mutex);
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    int ok = started == 4;
    for (unsigned i = 0; i < started; ++i) {
        printf("WSI_LIFECYCLE worker=%u cycles=%u expected=8\n", i, workers[i].completed);
        ok &= workers[i].completed == 8;
    }
    pthread_barrier_destroy(&gate.live);
    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);
    if (wl_display_roundtrip(display) < 0) return 2;
    int after = fd_count();
    printf("WSI_LIFECYCLE surfaces=128 concurrent=16 fd-before=%d fd-after=%d\n", before, after);
    return ok && before >= 0 && before == after ? 0 : 2;
}
