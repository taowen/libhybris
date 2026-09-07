#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "surface_lifecycle.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define V(name) PFN_##name name = (PFN_##name)gip(instance, #name); \
    if (!name) { printf("MISSING %s\n", #name); return 2; }
#define CHECK(call) do { VkResult result = (call); printf("%s = %d\n", #call, result); \
    if (result != VK_SUCCESS) return 2; } while (0)

struct window {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct xdg_wm_base *shell;
    int wlegl, configured, closed;
};
static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) { (void)data; xdg_wm_base_pong(base, serial); }
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    struct window *w = data;
    if (!strcmp(interface, "wl_compositor"))
        w->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
    if (!strcmp(interface, "xdg_wm_base")) {
        w->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->shell, &shell_listener, w);
    }
    if (!strcmp(interface, "android_wlegl")) w->wlegl = (int)version;
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {global, removed};
static void configured(void *data, struct xdg_surface *surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    ((struct window *)data)->configured = 1;
}
static const struct xdg_surface_listener surface_listener = {.configure = configured};
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
    (void)data; (void)toplevel; (void)states;
    printf("WSI configure width=%d height=%d\n", width, height);
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) { (void)toplevel; ((struct window *)data)->closed = 1; }
static const struct xdg_toplevel_listener toplevel_listener = {.configure = toplevel_configure, .close = toplevel_close};

static int has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return 1;
    return 0;
}

static void dump_maps(const char *phase) {
    char path[80];
    snprintf(path, sizeof(path), "maps-%s.txt", phase);
    FILE *input = fopen("/proc/self/maps", "r"), *output = fopen(path, "w");
    if (input && output) {
        char line[4096];
        while (fgets(line, sizeof(line), input)) fputs(line, output);
    }
    if (input) fclose(input);
    if (output) fclose(output);
}

static int icd_version(void) {
    alarm(20);
    void *library = dlopen("libhybris-vulkan-icd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr resolver = dlsym(library, "vk_icdGetInstanceProcAddr");
    PFN_vkEnumerateInstanceVersion query = resolver
        ? (PFN_vkEnumerateInstanceVersion)resolver(VK_NULL_HANDLE, "vkEnumerateInstanceVersion") : NULL;
    uint32_t version = 0;
    if (!query || query(&version) != VK_SUCCESS) return 2;
    printf("WSI_ICD_VERSION %u.%u.%u\n", VK_VERSION_MAJOR(version),
           VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    dlclose(library);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--icd-version")) return icd_version();
    if (argc != 1) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("WSI_CLIENT pid=%ld\n", (long)getpid());
    alarm(45);
    struct window w = {0};
    w.display = wl_display_connect(NULL);
    if (!w.display) { printf("WSI connect errno=%d\n", errno); return 2; }
    struct wl_registry *registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(registry, &registry_listener, &w);
    if (wl_display_roundtrip(w.display) < 0) return 2;
    printf("WSI globals compositor=%d xdg=%d android_wlegl=%d\n", w.compositor != NULL, w.shell != NULL, w.wlegl);
    if (!w.compositor || !w.shell) return 3;
    struct wl_surface *wl_surface = wl_compositor_create_surface(w.compositor);
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(w.shell, wl_surface);
    xdg_surface_add_listener(xdg_surface, &surface_listener, &w);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, &w);
    xdg_toplevel_set_title(toplevel, "libhybris ICD surface probe");
    xdg_toplevel_set_app_id(toplevel, "libhybris-icd-surface");
    wl_surface_commit(wl_surface);
    while (!w.configured && !w.closed) if (wl_display_dispatch(w.display) < 0) return 2;
    if (w.closed) return 2;
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) { printf("Vulkan dlopen: %s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr gip = dlsym(library, "vkGetInstanceProcAddr");
    if (!gip) return 2;
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)gip(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)gip(VK_NULL_HANDLE, "vkCreateInstance");
    if (!enumerate || !vkCreateInstance) return 2;
    uint32_t count = 0;
    CHECK(enumerate(NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return 2;
    CHECK(enumerate(NULL, &count, extensions));
    int surface_ext = has_extension(extensions, count, VK_KHR_SURFACE_EXTENSION_NAME);
    int wayland_ext = has_extension(extensions, count, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    printf("WSI_ICD surface_ext=%d wayland_ext=%d count=%u\n", surface_ext, wayland_ext, count);
    free(extensions);
    if (!surface_ext || !wayland_ext) return 2;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo bare = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance gated = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&bare, NULL, &gated));
    PFN_vkCreateWaylandSurfaceKHR ungated_create =
        (PFN_vkCreateWaylandSurfaceKHR)gip(gated, "vkCreateWaylandSurfaceKHR");
    printf("WSI_ICD ungated create=%s\n", ungated_create ? "NONNULL" : "NULL");
    /* A disabled instance-extension command must not resolve. Do not invoke
     * it through a standard loader with an invalid extension configuration. */
    if (ungated_create) return 2;
    PFN_vkDestroyInstance destroy_gated = (PFN_vkDestroyInstance)gip(gated, "vkDestroyInstance");
    if (!destroy_gated) return 2;
    destroy_gated(gated, NULL);
    const char *enabled[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = enabled};
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ci, NULL, &instance));
    dump_maps("instance");
    V(vkDestroyInstance); V(vkCreateWaylandSurfaceKHR); V(vkDestroySurfaceKHR);
    V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkGetPhysicalDeviceSurfaceSupportKHR); V(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    V(vkGetPhysicalDeviceSurfaceFormatsKHR); V(vkGetPhysicalDeviceSurfacePresentModesKHR);
    V(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    printf("WSI_ICD swapchain_gipa=%d\n", gip(instance, "vkCreateSwapchainKHR") != NULL);
    VkWaylandSurfaceCreateInfoKHR wc = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = w.display, .surface = wl_surface};
    if (!w.wlegl) {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            VkSurfaceKHR rejected = VK_NULL_HANDLE;
            VkResult result = vkCreateWaylandSurfaceKHR(instance, &wc, NULL, &rejected);
            printf("WSI_MISSING_WLEGL attempt=%u result=%d expected=%d\n",
                   attempt, result, VK_ERROR_UNKNOWN);
            if (result == VK_SUCCESS) vkDestroySurfaceKHR(instance, rejected, NULL);
            if (result != VK_ERROR_UNKNOWN) return 2;
        }
        vkDestroyInstance(instance, NULL);
        xdg_toplevel_destroy(toplevel);
        xdg_surface_destroy(xdg_surface);
        wl_surface_destroy(wl_surface);
        xdg_wm_base_destroy(w.shell);
        wl_compositor_destroy(w.compositor);
        wl_registry_destroy(registry);
        wl_display_disconnect(w.display);
        dlclose(library);
        printf("WSI_MISSING_WLEGL rejection=PASS window=UNSUPPORTED\n");
        return 3;
    }
    if (surface_lifecycle(w.display, w.compositor, instance, vkCreateWaylandSurfaceKHR, vkDestroySurfaceKHR)) return 2;
    VkSurfaceKHR surface;
    CHECK(vkCreateWaylandSurfaceKHR(instance, &wc, NULL, &surface));
    dump_maps("surface");
    count = 1;
    VkPhysicalDevice physical;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || !count) return 2;
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) return 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    if (!count) return 2;
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 present = vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical, i, w.display);
        VkBool32 supported = 0;
        CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported));
        printf("WSI_ICD family=%u graphics=%d present=%d support=%d\n", i,
               !!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT), present, supported);
        if (supported && present && families[i].queueCount &&
            (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family = i;
    }
    free(families);
    if (family == UINT32_MAX) {
        printf("WSI_ICD presentation=UNSUPPORTED\n");
        return 3;
    }
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    printf("WSI_ICD currentExtent=%ux%u minImage=%u maxImage=%u usage=0x%x\n",
           caps.currentExtent.width, caps.currentExtent.height,
           caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags);
    if (caps.currentExtent.width != 0xffffffffu || caps.currentExtent.height != 0xffffffffu) return 2;
    count = 0;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL));
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (!formats) return 2;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats));
    VkSurfaceFormatKHR format = {0};
    for (uint32_t i = 0; i < count; ++i)
        if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM))
            format = formats[i];
    free(formats);
    if (!format.format) return 3;
    count = 0;
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, NULL));
    VkPresentModeKHR *modes = calloc(count, sizeof(*modes));
    if (!modes) return 2;
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes));
    int fifo = 0;
    for (uint32_t i = 0; i < count; ++i) fifo |= modes[i] == VK_PRESENT_MODE_FIFO_KHR;
    free(modes);
    if (!fifo) return 2;
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg_surface);
    wl_surface_destroy(wl_surface);
    xdg_wm_base_destroy(w.shell);
    wl_compositor_destroy(w.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(w.display);
    dlclose(library);
    printf("WSI_ICD surface=PASS presentation=ADVERTISED fifo=1 format=%u\n", format.format);
    return 0;
}
