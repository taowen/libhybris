/* SPDX-License-Identifier: Apache-2.0 */
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#define V(name) PFN_##name name = (PFN_##name)gip(instance, #name); if (!name) { printf("X11_MISSING %s\n", #name); return 2; }
#define OK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { printf("X11_VK_ERROR %s result=%d\n", #call, r); return 2; } } while (0)
static atomic_uint validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        atomic_fetch_add(&validation_errors, 1);
        printf("VALIDATION %s: %s\n", data->pMessageIdName ? data->pMessageIdName : "(unnamed)", data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}
int x11_render(PFN_vkGetInstanceProcAddr gip, xcb_connection_t *connection, xcb_window_t window, xcb_visualid_t visual, Display *display) {
    int validate = getenv("HYBRIS_X11_VALIDATION") != NULL;
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, display ? VK_KHR_XLIB_SURFACE_EXTENSION_NAME : VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &sync};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (validate) { ic.enabledExtensionCount = 4; ic.pNext = &features; ic.enabledLayerCount = 1; ic.ppEnabledLayerNames = &layer; }
    OK(vkCreateInstance(&ic, NULL, &instance));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug = NULL;
    if (validate) {
        V(vkCreateDebugUtilsMessengerEXT);
        destroy_debug = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!destroy_debug) return 2;
        OK(vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
    }
    V(vkDestroyInstance); V(vkDestroySurfaceKHR);
    PFN_vkCreateXcbSurfaceKHR vkCreateXcbSurfaceKHR = (PFN_vkCreateXcbSurfaceKHR)gip(instance, "vkCreateXcbSurfaceKHR");
    PFN_vkCreateXlibSurfaceKHR vkCreateXlibSurfaceKHR = (PFN_vkCreateXlibSurfaceKHR)gip(instance, "vkCreateXlibSurfaceKHR");
    if (display ? !vkCreateXlibSurfaceKHR || vkCreateXcbSurfaceKHR : !vkCreateXcbSurfaceKHR || vkCreateXlibSurfaceKHR) return 2;
    V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkGetPhysicalDeviceSurfaceSupportKHR);
    PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR xcb_support = (PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR)gip(instance, "vkGetPhysicalDeviceXcbPresentationSupportKHR");
    PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR xlib_support = (PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR)gip(instance, "vkGetPhysicalDeviceXlibPresentationSupportKHR");
    if (display ? !xlib_support || xcb_support : !xcb_support || xlib_support) return 2;
    V(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); V(vkGetPhysicalDeviceSurfaceFormatsKHR);
    V(vkGetPhysicalDeviceMemoryProperties); V(vkCreateDevice); V(vkDestroyDevice); V(vkGetDeviceQueue);
    V(vkCreateSwapchainKHR); V(vkDestroySwapchainKHR); V(vkGetSwapchainImagesKHR);
    V(vkAcquireNextImageKHR); V(vkQueuePresentKHR); V(vkQueueSubmit); V(vkQueueWaitIdle);
    V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements);
    V(vkAllocateMemory); V(vkFreeMemory); V(vkBindBufferMemory); V(vkMapMemory); V(vkUnmapMemory);
    V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkAllocateCommandBuffers); V(vkResetCommandBuffer);
    V(vkBeginCommandBuffer); V(vkEndCommandBuffer); V(vkCmdPipelineBarrier); V(vkCmdClearColorImage); V(vkCmdCopyImageToBuffer);
    V(vkCreateSemaphore); V(vkDestroySemaphore); V(vkCreateFence); V(vkDestroyFence); V(vkWaitForFences); V(vkResetFences); V(vkGetFenceStatus);
    uint32_t count = 0; OK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) return 3;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices)); if (!devices) return 2;
    OK(vkEnumeratePhysicalDevices(instance, &count, devices)); VkPhysicalDevice physical = devices[0]; free(devices);
    VkXcbSurfaceCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = connection, .window = window};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (getenv("HYBRIS_X11_EXPECT_MISSING")) {
        VkBool32 support = display ? xlib_support(physical, 0, display, visual) : xcb_support(physical, 0, connection, visual);
        if (support) return 2;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            VkResult rejected;
            if (display) {
                VkXlibSurfaceCreateInfoKHR xc = {.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .dpy = display, .window = window};
                rejected = vkCreateXlibSurfaceKHR(instance, &xc, NULL, &surface);
            } else rejected = vkCreateXcbSurfaceKHR(instance, &sc, NULL, &surface);
            printf("X11_REJECT attempt=%u result=%d\n", attempt, rejected);
            if (rejected != VK_ERROR_UNKNOWN) return 2;
        }
        if (destroy_debug) destroy_debug(instance, messenger, NULL);
        vkDestroyInstance(instance, NULL);
        if (validate) printf("X11_VALIDATION errors=%u\n", atomic_load(&validation_errors));
        return atomic_load(&validation_errors) ? 2 : 0;
    }
    if (display) {
        VkXlibSurfaceCreateInfoKHR xc = {.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .dpy = display, .window = window};
        OK(vkCreateXlibSurfaceKHR(instance, &xc, NULL, &surface));
    } else { OK(vkCreateXcbSurfaceKHR(instance, &sc, NULL, &surface)); }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families)); if (!families) return 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 supported; OK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported));
        if (supported && (display ? xlib_support(physical, i, display, visual) : xcb_support(physical, i, connection, visual)) &&
            families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    }
    free(families); if (family == UINT32_MAX) return 3;
    float priority = 1; const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qc, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &swapchain_extension};
    VkDevice device; OK(vkCreateDevice(physical, &dc, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkSurfaceCapabilitiesKHR caps; OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    if (caps.currentExtent.width != 320 || caps.currentExtent.height != 240) return 2;
    OK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL));
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats)); if (!formats) return 2;
    OK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats));
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < count; ++i)
        if ((formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format = formats[i].format; break; }
    free(formats); if (!format) return 3;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & usage) != usage || caps.minImageCount > 3 || (caps.maxImageCount && caps.maxImageCount < 3)) return 3;
    VkSwapchainCreateInfoKHR sw = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
        .minImageCount = 3, .imageFormat = format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {320, 240}, .imageArrayLayers = 1, .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR};
    VkSwapchainKHR chain; OK(vkCreateSwapchainKHR(device, &sw, NULL, &chain));
    OK(vkGetSwapchainImagesKHR(device, chain, &count, NULL));
    VkImage *images = calloc(count, sizeof(*images)); VkSemaphore *ready = calloc(count, sizeof(*ready));
    if (!images || !ready) return 2;
    OK(vkGetSwapchainImagesKHR(device, chain, &count, images));
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acquired;
    OK(vkCreateSemaphore(device, &sem, NULL, &acquired));
    for (uint32_t i = 0; i < count; ++i) OK(vkCreateSemaphore(device, &sem, NULL, &ready[i]));
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 320 * 240 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer; OK(vkCreateBuffer(device, &bc, NULL, &buffer));
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties; vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
    if (type == UINT32_MAX) return 3;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    VkDeviceMemory memory; OK(vkAllocateMemory(device, &ma, NULL, &memory)); OK(vkBindBufferMemory(device, buffer, memory, 0));
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool pool; OK(vkCreateCommandPool(device, &pc, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; OK(vkAllocateCommandBuffers(device, &ca, &command));
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    OK(vkCreateFence(device, &fc, NULL, &fence));
    if (getenv("HYBRIS_X11_ACQUIRE_TIMEOUT")) {
        if (count > 8) return 2;
        uint32_t mask = 0;
        for (unsigned i = 0; i < count; ++i) {
            uint32_t index;
            OK(vkAcquireNextImageKHR(device, chain, 2000000000ull, VK_NULL_HANDLE, fence, &index));
            if (index >= count || (mask & (1u << index))) return 2;
            mask |= 1u << index;
            OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
            OK(vkResetFences(device, 1, &fence));
        }
        uint32_t index = UINT32_MAX;
        if (vkAcquireNextImageKHR(device, chain, 0, VK_NULL_HANDLE, fence, &index) != VK_NOT_READY || index != UINT32_MAX) return 2;
        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        VkResult result = vkAcquireNextImageKHR(device, chain, 20000000, VK_NULL_HANDLE, fence, &index);
        clock_gettime(CLOCK_MONOTONIC, &after);
        int64_t elapsed = (after.tv_sec - before.tv_sec) * 1000000000LL + after.tv_nsec - before.tv_nsec;
        if (result != VK_TIMEOUT || index != UINT32_MAX || elapsed < 15000000 || elapsed > 2000000000LL || vkGetFenceStatus(device, fence) != VK_NOT_READY) return 2;
        printf("X11_ACQUIRE held=%u zero=NOT_READY finite=TIMEOUT elapsed_ns=%lld index_unchanged=1 fence_unsignaled=1\n", count, (long long)elapsed);
        goto finish;
    }
    for (unsigned frame = 0; frame < 8; ++frame) {
        uint32_t index; OK(vkAcquireNextImageKHR(device, chain, 2000000000ull, acquired, VK_NULL_HANDLE, &index));
        if (index >= count) return 2;
        OK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        OK(vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkClearColorValue color = {.float32 = {frame & 1 ? 1 : 0, frame & 1 ? 0 : 1, 0, 1}};
        vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {320, 240, 1}};
        vkCmdCopyImageToBuffer(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
        OK(vkEndCommandBuffer(command));
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &wait, .commandBufferCount = 1, .pCommandBuffers = &command,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &ready[index]};
        OK(vkQueueSubmit(queue, 1, &submit, fence)); OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
        unsigned char *pixels; OK(vkMapMemory(device, memory, 0, 320 * 240 * 4, 0, (void **)&pixels));
        unsigned char rgba[320 * 240 * 4];
        for (unsigned i = 0; i < 320 * 240; ++i) {
            unsigned char *p = rgba + 4 * i;
            p[0] = pixels[4*i+(format == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 0)]; p[1] = pixels[4*i+1];
            p[2] = pixels[4*i+(format == VK_FORMAT_B8G8R8A8_UNORM ? 0 : 2)]; p[3] = pixels[4*i+3];
            if (p[0] != (frame & 1 ? 255 : 0) || p[1] != (frame & 1 ? 0 : 255) || p[2] || p[3] != 255) return 2;
        }
        vkUnmapMemory(device, memory);
        if (frame == 0 || frame == 7) {
            char path[32]; snprintf(path, sizeof(path), "image-0-%u.rgba", frame);
            FILE *file = fopen(path, "wb"); if (!file) return 2;
            size_t bytes = fwrite(rgba, 1, sizeof(rgba), file); if (fclose(file) || bytes != sizeof(rgba)) return 2;
        }
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &ready[index], .swapchainCount = 1, .pSwapchains = &chain, .pImageIndices = &index};
        OK(vkQueuePresentKHR(queue, &pi)); OK(vkResetFences(device, 1, &fence));
        printf("X11_FRAME frame=%u image=%u pixels=76800 exact=1\n", frame, index);
        if (frame == 0 || frame == 7) sleep(1);
    }
finish: ;
    FILE *maps = fopen("/proc/self/maps", "r"), *saved = fopen("maps.txt", "w");
    if (!maps || !saved) return 2;
    char line[2048]; while (fgets(line, sizeof(line), maps)) if (fputs(line, saved) < 0) return 2;
    int bad = ferror(maps); fclose(maps); if (fclose(saved) || bad) return 2;
    OK(vkQueueWaitIdle(queue));
    vkDestroyFence(device, fence, NULL); vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL);
    for (uint32_t i = 0; i < count; ++i) vkDestroySemaphore(device, ready[i], NULL);
    vkDestroySemaphore(device, acquired, NULL); free(images); free(ready);
    vkDestroySwapchainKHR(device, chain, NULL); vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    if (destroy_debug) destroy_debug(instance, messenger, NULL);
    vkDestroyInstance(instance, NULL);
    if (validate) printf("X11_VALIDATION errors=%u\n", atomic_load(&validation_errors));
    return atomic_load(&validation_errors) ? 2 : 0;
}
