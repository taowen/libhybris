#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "surface_lifecycle.h"
#include "swapchain_review.h"
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
    int wlegl, configured, frame, closed;
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
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time; ((struct window *)data)->frame = 1; wl_callback_destroy(callback);
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static unsigned validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)types;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        printf("VALIDATION %s: %s\n", data->pMessageIdName ? data->pMessageIdName : "(unnamed)",
               data->pMessage ? data->pMessage : "");
        ++validation_errors;
    }
    return VK_FALSE;
}

static int has_layer(PFN_vkEnumerateInstanceLayerProperties enumerate, const char *name)
{
    uint32_t count = 0;
    if (!enumerate || enumerate(&count, NULL) != VK_SUCCESS) return 0;
    VkLayerProperties *layers = calloc(count, sizeof(*layers));
    if (count && !layers) return 0;
    if (enumerate(&count, layers) != VK_SUCCESS) { free(layers); return 0; }
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) found |= !strcmp(layers[i].layerName, name);
    free(layers);
    return found;
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
    int review = getenv("HYBRIS_WSI_SWAPCHAIN_REVIEW") != NULL;
    int validate = getenv("HYBRIS_WSI_VALIDATION") != NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("WSI_CLIENT pid=%ld\n", (long)getpid());
    alarm(validate || getenv("GFXRECON_CAPTURE_FILE") ? 180 : 45);
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
    xdg_toplevel_set_title(toplevel, "libhybris WSI probe");
    xdg_toplevel_set_app_id(toplevel, "libhybris-wsi-probe");
    wl_surface_commit(wl_surface);
    while (!w.configured && !w.closed) if (wl_display_dispatch(w.display) < 0) return 2;
    if (w.closed) return 2;
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) { printf("Vulkan dlopen: %s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr gip = dlsym(library, "vkGetInstanceProcAddr");
    if (!gip) return 2;
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[4] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    PFN_vkEnumerateInstanceLayerProperties enumerate_layers =
        (PFN_vkEnumerateInstanceLayerProperties)gip(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    if (validate && !has_layer(enumerate_layers, layer)) {
        printf("WSI_VALIDATION layer=MISSING\n");
        return 2;
    }
    VkValidationFeatureEnableEXT syncval = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation_message};
    VkValidationFeaturesEXT features = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &syncval};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = review ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    if (validate) {
        ci.pNext = &features;
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        ci.enabledExtensionCount = 4;
    }
    CHECK(vkCreateInstance(&ci, NULL, &instance));
    dump_maps("instance");
    V(vkDestroyInstance); V(vkCreateWaylandSurfaceKHR); V(vkDestroySurfaceKHR);
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    if (validate) {
        V(vkCreateDebugUtilsMessengerEXT);
        destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!destroy_messenger) return 2;
        CHECK(vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
    }
    V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkGetPhysicalDeviceSurfaceSupportKHR); V(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    V(vkGetPhysicalDeviceSurfaceFormatsKHR); V(vkGetPhysicalDeviceMemoryProperties);
    V(vkCreateDevice); V(vkDestroyDevice); V(vkGetDeviceQueue);
    V(vkCreateSwapchainKHR); V(vkDestroySwapchainKHR); V(vkGetSwapchainImagesKHR);
    V(vkAcquireNextImageKHR); V(vkQueuePresentKHR); V(vkQueueSubmit); V(vkQueueWaitIdle);
    V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements);
    V(vkAllocateMemory); V(vkFreeMemory); V(vkBindBufferMemory); V(vkMapMemory); V(vkUnmapMemory);
    V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkAllocateCommandBuffers);
    V(vkBeginCommandBuffer); V(vkEndCommandBuffer); V(vkResetCommandBuffer);
    V(vkCmdPipelineBarrier); V(vkCmdClearColorImage); V(vkCmdCopyImageToBuffer);
    V(vkCreateSemaphore); V(vkDestroySemaphore); V(vkCreateFence); V(vkDestroyFence);
    V(vkWaitForFences); V(vkResetFences);
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
        if (destroy_messenger && messenger) destroy_messenger(instance, messenger, NULL);
        printf("WSI_VALIDATION errors=%u\n", validation_errors);
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
        return validation_errors ? 2 : 3;
    }
    if (surface_lifecycle(w.display, w.compositor, instance, vkCreateWaylandSurfaceKHR, vkDestroySurfaceKHR)) return 2;
    VkSurfaceKHR surface;
    CHECK(vkCreateWaylandSurfaceKHR(instance, &wc, NULL, &surface));
    dump_maps("surface");
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || !count) return 2;
    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) return 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 supported = 0;
        CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported));
        if (supported && families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    }
    free(families);
    if (family == UINT32_MAX) return 3;
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qc, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &swapchain_extension};
    VkDevice device;
    CHECK(vkCreateDevice(physical, &dc, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    const VkExtent2D sizes[] = {{320, 240}, {448, 288}, {256, 192}};
    VkSwapchainKHR previous = VK_NULL_HANDLE;
    for (unsigned epoch = 0; epoch < sizeof(sizes) / sizeof(sizes[0]); ++epoch) {
        const uint32_t width = sizes[epoch].width, height = sizes[epoch].height;
        xdg_surface_set_window_geometry(xdg_surface, 0, 0, width, height);
        printf("WSI_RESIZE epoch=%u size=%ux%u old-swapchain=%d\n", epoch, width, height, previous != VK_NULL_HANDLE);
        VkSurfaceCapabilitiesKHR caps;
        CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
        if (width < caps.minImageExtent.width || width > caps.maxImageExtent.width ||
            height < caps.minImageExtent.height || height > caps.maxImageExtent.height) return 3;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((caps.supportedUsageFlags & usage) != usage) { printf("UNSUPPORTED transfer swapchain usage=%x\n", caps.supportedUsageFlags); return 3; }
        count = 0;
        CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL));
        VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
        if (!formats) return 2;
        CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats));
        VkSurfaceFormatKHR format = {0};
        for (uint32_t i = 0; i < count; ++i)
            if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)) { format = formats[i]; break; }
        free(formats);
        if (!format.format) return 3;
        printf("WSI format=%u colorSpace=%u image-count-min=%u max=%u\n", format.format, format.colorSpace, caps.minImageCount, caps.maxImageCount);
        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
        VkCompositeAlphaFlagBitsKHR alpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface, .minImageCount = image_count, .imageFormat = format.format,
            .imageColorSpace = format.colorSpace, .imageExtent = {width, height}, .imageArrayLayers = 1,
            .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform, .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_FALSE, .oldSwapchain = previous};
        if (review && epoch == 0 && swapchain_review(gip, instance, physical, device, queue, family, &sc)) return 2;
        VkSwapchainKHR swapchain;
        CHECK(vkCreateSwapchainKHR(device, &sc, NULL, &swapchain));
        if (previous) vkDestroySwapchainKHR(device, previous, NULL);
        previous = swapchain;
        CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
        VkImage *images = calloc(image_count, sizeof(*images));
        VkSemaphore *rendered = calloc(image_count, sizeof(*rendered));
        if (!images || !rendered) return 2;
        CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));
        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acquired;
        CHECK(vkCreateSemaphore(device, &sem, NULL, &acquired));
        for (uint32_t i = 0; i < image_count; ++i) CHECK(vkCreateSemaphore(device, &sem, NULL, &rendered[i]));
        VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = width * height * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer buffer;
        CHECK(vkCreateBuffer(device, &bc, NULL, &buffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { memory_type = i; break; }
        if (memory_type == UINT32_MAX) return 3;
        VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = memory_type};
        VkDeviceMemory memory;
        CHECK(vkAllocateMemory(device, &ma, NULL, &memory));
        CHECK(vkBindBufferMemory(device, buffer, memory, 0));
        VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
        VkCommandPool pool;
        CHECK(vkCreateCommandPool(device, &pc, NULL, &pool));
        VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer command;
        CHECK(vkAllocateCommandBuffers(device, &ca, &command));
        VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        CHECK(vkCreateFence(device, &fc, NULL, &fence));
        for (unsigned frame = 0; frame < 8; ++frame) {
            uint32_t index;
            CHECK(vkAcquireNextImageKHR(device, swapchain, 5000000000ull, acquired, VK_NULL_HANDLE, &index));
            CHECK(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(vkBeginCommandBuffer(command, &begin));
            VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &barrier);
            VkClearColorValue color = {.float32 = {frame & 1 ? 1 : 0, frame & 1 ? 0 : 1, 0, 1}};
            vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {width, height, 1}};
            vkCmdCopyImageToBuffer(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
            VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
            CHECK(vkEndCommandBuffer(command));
            VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                .pWaitSemaphores = &acquired, .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1,
                .pCommandBuffers = &command, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered[index]};
            CHECK(vkQueueSubmit(queue, 1, &submit, fence));
            CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
            unsigned char *pixels;
            CHECK(vkMapMemory(device, memory, 0, width * height * 4, 0, (void **)&pixels));
            const unsigned char expected[4] = {frame & 1 ? (format.format == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 255) : 0,
                frame & 1 ? 0 : 255, frame & 1 && format.format == VK_FORMAT_B8G8R8A8_UNORM ? 255 : 0, 255};
            unsigned mismatch = 0;
            for (uint32_t pixel = 0; pixel < width * height; ++pixel) mismatch += memcmp(pixels + pixel * 4, expected, 4) != 0;
            if (frame == 0 || frame == 7) {
                char path[80];
                snprintf(path, sizeof(path), "image-%u-%u.rgba", epoch, frame);
                FILE *image = fopen(path, "wb");
                if (!image) return 2;
                int written = 1;
                if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
                    for (uint32_t i = 0; i < width * height; ++i) {
                        unsigned char rgba[] = {pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4], pixels[i * 4 + 3]};
                        written &= fwrite(rgba, 1, 4, image) == 4;
                    }
                } else written = fwrite(pixels, 1, width * height * 4, image) == width * height * 4;
                if (fclose(image) || !written) return 2;
            }
            vkUnmapMemory(device, memory);
            if (mismatch) { printf("WSI readback mismatches=%u\n", mismatch); return 2; }
            w.frame = 0;
            struct wl_callback *callback = wl_surface_frame(wl_surface);
            wl_callback_add_listener(callback, &frame_listener, &w);
            VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered[index], .swapchainCount = 1,
                .pSwapchains = &swapchain, .pImageIndices = &index};
            CHECK(vkQueuePresentKHR(queue, &present));
            while (!w.frame && !w.closed) if (wl_display_dispatch(w.display) < 0) return 2;
            if (w.closed) return 2;
            printf("WSI_FRAME epoch=%u frame=%u image=%u size=%ux%u rgba=%s readback=exact callback=1\n",
                epoch, frame, index, width, height, frame & 1 ? "255,0,0,255" : "0,255,0,255");
            CHECK(vkResetFences(device, 1, &fence));
            if (frame == 0 && epoch == 0) dump_maps("frame");
            if (frame == 0 || frame == 7) sleep(2);
        }
        /* Rebuild/teardown boundary; no wait-idle in the per-frame path. */
        CHECK(vkQueueWaitIdle(queue));
        vkDestroyFence(device, fence, NULL);
        vkDestroyCommandPool(device, pool, NULL);
        vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL);
        vkDestroySemaphore(device, acquired, NULL);
        for (uint32_t i = 0; i < image_count; ++i) vkDestroySemaphore(device, rendered[i], NULL);
        free(rendered); free(images);
    }
    vkDestroySwapchainKHR(device, previous, NULL);
    vkDestroyDevice(device, NULL); vkDestroySurfaceKHR(instance, surface, NULL);
    if (destroy_messenger && messenger) destroy_messenger(instance, messenger, NULL);
    printf("WSI_VALIDATION errors=%u\n", validation_errors);
    if (validation_errors) return 2;
    vkDestroyInstance(instance, NULL);
    xdg_toplevel_destroy(toplevel); xdg_surface_destroy(xdg_surface); wl_surface_destroy(wl_surface);
    xdg_wm_base_destroy(w.shell); wl_compositor_destroy(w.compositor); wl_registry_destroy(registry);
    wl_display_disconnect(w.display);
    dlclose(library);
    printf("WSI PASS\n");
    return 0;
}
