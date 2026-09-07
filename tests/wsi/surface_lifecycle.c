#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "surface_lifecycle.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Checks retained from the retired surface-only executable. They now run in
 * the same client that creates swapchains and verifies displayed pixels. */
int surface_extension_checks(PFN_vkGetInstanceProcAddr gip) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate = (PFN_vkEnumerateInstanceExtensionProperties)gip(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gip(VK_NULL_HANDLE, "vkCreateInstance");
    uint32_t count = 0;
    if (!enumerate || !create || enumerate(NULL, &count, NULL) != VK_SUCCESS) return 2;
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return 2;
    VkResult result = enumerate(NULL, &count, extensions);
    int surface = 0, wayland = 0;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; ++i) {
        surface |= !strcmp(extensions[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
        wayland |= !strcmp(extensions[i].extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    }
    free(extensions);
    if (!surface || !wayland) return 2;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    if (create(&info, NULL, &instance) != VK_SUCCESS) return 2;
    int disabled = gip(instance, "vkCreateWaylandSurfaceKHR") == NULL;
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gip(instance, "vkDestroyInstance");
    if (!destroy) return 2;
    destroy(instance, NULL);
    printf("WSI_SURFACE_EXTENSIONS surface=%d wayland=%d disabled_create_null=%d\n", surface, wayland, disabled);
    return disabled ? 0 : 2;
}

int surface_presentation_checks(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface, struct wl_display *display) {
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR support = (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)gip(instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gip(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gip(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceCapabilitiesKHR caps;
    if (!capabilities || capabilities(physical, surface, &caps) != VK_SUCCESS ||
        caps.currentExtent.width != UINT32_MAX || caps.currentExtent.height != UINT32_MAX) return 2;
    uint32_t count = 0;
    if (!support || !modes || !support(physical, family, display) || modes(physical, surface, &count, NULL) != VK_SUCCESS) return 2;
    VkPresentModeKHR *values = calloc(count, sizeof(*values));
    if (!values) return 2;
    VkResult result = modes(physical, surface, &count, values);
    int fifo = 0;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; ++i) fifo |= values[i] == VK_PRESENT_MODE_FIFO_KHR;
    free(values);
    printf("WSI_SURFACE_PRESENT support=1 extent_app_selected=1 fifo=%d\n", fifo);
    return fifo ? 0 : 2;
}
