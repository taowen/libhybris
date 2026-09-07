/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "config.h"
#ifdef WANT_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include "swapchain.h"
#include "device.h"
#include "wsi.h"
#include "vk_android_native_buffer.h"
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef WANT_WAYLAND
#include <hybris/gralloc/gralloc.h>
#include <hybris/grallocusage/GrallocUsageConversion.h>
#include <system/graphics.h>
#include <system/window.h>
#include "../platforms/wayland/window_owner.h"
#endif

#ifdef WANT_WAYLAND
enum image_state { IMAGE_FREE, IMAGE_ACQUIRED, IMAGE_PRESENTED };

struct swapchain_image {
    VkImage image;
    struct ANativeWindowBuffer *native;
    enum image_state state;
};

struct swapchain_state {
    VkDevice device;
    uint64_t device_generation;
    VkSurfaceKHR surface;
    struct hybris_vk_wayland_window *window;
    uint32_t count;
    struct swapchain_image *images;
    int retired;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct swapchain_state *next;
};

static pthread_mutex_t swapchain_guard = PTHREAD_MUTEX_INITIALIZER;
static struct swapchain_state *swapchains;

static void *object_alloc(const VkAllocationCallbacks *allocator, size_t size,
    VkSystemAllocationScope scope)
{
    return allocator
        ? allocator->pfnAllocation(allocator->pUserData, size, 8, scope)
        : malloc(size);
}

static void object_free(const VkAllocationCallbacks *allocator, int custom, void *memory)
{
    if (!memory) return;
    if (custom) allocator->pfnFree(allocator->pUserData, memory);
    else free(memory);
}

static struct swapchain_state *unlink_swapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    pthread_mutex_lock(&swapchain_guard);
    struct swapchain_state **link = &swapchains;
    while (*link && (VkSwapchainKHR)(uintptr_t)*link != swapchain)
        link = &(*link)->next;
    struct swapchain_state *state = *link;
    if (state && state->device == device) *link = state->next;
    else state = NULL;
    pthread_mutex_unlock(&swapchain_guard);
    return state;
}

static struct swapchain_state *find_swapchain(VkSwapchainKHR swapchain)
{
    pthread_mutex_lock(&swapchain_guard);
    struct swapchain_state *state = swapchains;
    while (state && (VkSwapchainKHR)(uintptr_t)state != swapchain)
        state = state->next;
    pthread_mutex_unlock(&swapchain_guard);
    return state;
}

static void retire_swapchain(VkSwapchainKHR swapchain)
{
    struct hybris_vk_wayland_window *window = NULL;
    pthread_mutex_lock(&swapchain_guard);
    struct swapchain_state *state = swapchains;
    while (state && (VkSwapchainKHR)(uintptr_t)state != swapchain)
        state = state->next;
    if (state) {
        state->retired = 1;
        window = state->window;
    }
    pthread_mutex_unlock(&swapchain_guard);
    if (window) hybris_vk_wayland_window_disconnect(window);
}

static int hal_format(VkFormat format)
{
    if (format == VK_FORMAT_R8G8B8A8_UNORM) return HAL_PIXEL_FORMAT_RGBA_8888;
    if (format == VK_FORMAT_B8G8R8A8_UNORM) return HAL_PIXEL_FORMAT_BGRA_8888;
    return 0;
}

static VkResult import_image(const struct hybris_icd_device *device,
    const VkSwapchainCreateInfoKHR *info, struct ANativeWindowBuffer *native,
    int usage, VkImage *image)
{
    PFN_vkCreateImage create = (PFN_vkCreateImage)device->resolver(device->handle, "vkCreateImage");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSwapchainImageCreateInfoANDROID swapchain_image = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID};
    VkNativeBufferANDROID buffer = {
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
        .pNext = &swapchain_image,
        .handle = native->handle,
        .stride = native->stride,
        .format = native->format,
        .usage = usage,
        .usage3 = (uint32_t)usage,
        .ahb = hybris_gralloc_get_hardware_buffer(native->handle)};
    android_convertGralloc0To1Usage(usage, &buffer.usage2.producer, &buffer.usage2.consumer);
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &buffer,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = info->imageFormat,
        .extent = {info->imageExtent.width, info->imageExtent.height, 1},
        .mipLevels = 1,
        .arrayLayers = info->imageArrayLayers ? info->imageArrayLayers : 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = info->imageUsage,
        .sharingMode = info->imageSharingMode,
        .queueFamilyIndexCount = info->queueFamilyIndexCount,
        .pQueueFamilyIndices = info->pQueueFamilyIndices,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    return create(device->handle, &image_info, NULL, image);
}

static void destroy_images(const struct hybris_icd_device *device, struct swapchain_state *state)
{
    PFN_vkDestroyImage destroy = device
        ? (PFN_vkDestroyImage)device->resolver(device->handle, "vkDestroyImage") : NULL;
    for (uint32_t i = 0; i < state->count; ++i) {
        if (state->images[i].image && destroy)
            destroy(state->device, state->images[i].image, state->custom_allocator ? &state->allocator : NULL);
        if (state->images[i].native && !state->retired && state->window) {
            if (state->images[i].state != IMAGE_PRESENTED)
                hybris_vk_wayland_window_cancel(state->window, state->images[i].native, -1);
        }
        state->images[i].image = VK_NULL_HANDLE;
        state->images[i].native = NULL;
    }
}

static VkResult VKAPI_CALL create_swapchain(VkDevice device,
    const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator,
    VkSwapchainKHR *swapchain)
{
    struct hybris_icd_device context;
    if (!info || !swapchain || info->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!hybris_icd_lookup_device(device, &context) || !context.swapchain_enabled)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!info->imageExtent.width || !info->imageExtent.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    int pixel = hal_format(info->imageFormat);
    if (!pixel) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkInstance instance = VK_NULL_HANDLE;
    uint64_t generation = 0;
    struct hybris_vk_wayland_window *window =
        hybris_icd_wsi_surface_window(info->surface, &instance, &generation);
    if (!window) return VK_ERROR_SURFACE_LOST_KHR;
    PFN_vkGetSwapchainGrallocUsage2ANDROID usage2 =
        (PFN_vkGetSwapchainGrallocUsage2ANDROID)context.resolver(device,
            "vkGetSwapchainGrallocUsage2ANDROID");
    if (!usage2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint64_t consumer = 0, producer = 0;
    VkResult result = usage2(device, info->imageFormat, info->imageUsage, 0, &consumer, &producer);
    if (result != VK_SUCCESS) return result;
    int usage = android_convertGralloc1To0Usage(producer, consumer);
    uint32_t count = info->minImageCount;
    if (count < 2) count = 2;
    if (count > 8) count = 8;
    if (info->oldSwapchain) retire_swapchain(info->oldSwapchain);
    hybris_vk_wayland_window_resize(window, info->imageExtent.width, info->imageExtent.height);
    ANativeWindow *native = hybris_vk_wayland_window_native(window);
    native->perform(native, NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS,
        (int)info->imageExtent.width, (int)info->imageExtent.height);
    native->perform(native, NATIVE_WINDOW_SET_BUFFERS_FORMAT, pixel);
    native->perform(native, NATIVE_WINDOW_SET_USAGE, usage);
    native->perform(native, NATIVE_WINDOW_SET_BUFFER_COUNT, (int)count);
    struct swapchain_state *state = object_alloc(allocator, sizeof(*state),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(state, 0, sizeof(*state));
    state->device = device;
    state->device_generation = context.generation;
    state->surface = info->surface;
    state->window = window;
    state->custom_allocator = allocator != NULL;
    if (allocator) state->allocator = *allocator;
    state->images = object_alloc(allocator, count * sizeof(*state->images),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!state->images) {
        object_free(allocator, state->custom_allocator, state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(state->images, 0, count * sizeof(*state->images));
    state->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        struct ANativeWindowBuffer *buffer = NULL;
        int fence = -1;
        int error = hybris_vk_wayland_window_dequeue(window, 5000000000LL, &buffer, &fence);
        if (error || !buffer) {
            if (fence >= 0) close(fence);
            destroy_images(&context, state);
            object_free(allocator, state->custom_allocator, state->images);
            object_free(allocator, state->custom_allocator, state);
            return error == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN;
        }
        /* Create-time dequeue fence is unused; the driver is not waiting yet. */
        if (fence >= 0) close(fence);
        result = import_image(&context, info, buffer, usage, &state->images[i].image);
        if (result != VK_SUCCESS) {
            hybris_vk_wayland_window_cancel(window, buffer, -1);
            destroy_images(&context, state);
            object_free(allocator, state->custom_allocator, state->images);
            object_free(allocator, state->custom_allocator, state);
            return result;
        }
        state->images[i].native = buffer;
        state->images[i].state = IMAGE_FREE;
    }
    pthread_mutex_lock(&swapchain_guard);
    state->next = swapchains;
    swapchains = state;
    pthread_mutex_unlock(&swapchain_guard);
    *swapchain = (VkSwapchainKHR)(uintptr_t)state;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!device || !swapchain) return;
    struct swapchain_state *state = unlink_swapchain(device, swapchain);
    if (!state) return;
    struct hybris_icd_device context;
    destroy_images(hybris_icd_lookup_device(device, &context) ? &context : NULL, state);
    object_free(&state->allocator, state->custom_allocator, state->images);
    object_free(&state->allocator, state->custom_allocator, state);
}

static VkResult VKAPI_CALL get_swapchain_images(VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *count, VkImage *images)
{
    struct hybris_icd_device context;
    if (!count || !hybris_icd_lookup_device(device, &context))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct swapchain_state *state = find_swapchain(swapchain);
    if (!state || state->device != device) return VK_ERROR_SURFACE_LOST_KHR;
    if (!images) {
        *count = state->count;
        return VK_SUCCESS;
    }
    uint32_t written = *count < state->count ? *count : state->count;
    for (uint32_t i = 0; i < written; ++i) images[i] = state->images[i].image;
    *count = written;
    return written < state->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static int64_t dequeue_timeout(uint64_t timeout)
{
    if (timeout == 0) return 0;
    if (timeout == UINT64_MAX) return -1;
    if (timeout > (uint64_t)INT64_MAX) return INT64_MAX;
    return (int64_t)timeout;
}

static VkResult acquire_slot(struct swapchain_state *state, int64_t timeout_ns, uint32_t *index,
    int *fence)
{
    *fence = -1;
    for (uint32_t i = 0; i < state->count; ++i)
        if (state->images[i].state == IMAGE_FREE) {
            *index = i;
            return VK_SUCCESS;
        }
    struct ANativeWindowBuffer *buffer = NULL;
    int error = hybris_vk_wayland_window_dequeue(state->window, timeout_ns, &buffer, fence);
    if (error == -EAGAIN) return VK_NOT_READY;
    if (error == -ETIMEDOUT) return VK_TIMEOUT;
    if (error || !buffer) {
        if (*fence >= 0) { close(*fence); *fence = -1; }
        return VK_ERROR_UNKNOWN;
    }
    for (uint32_t i = 0; i < state->count; ++i)
        if (state->images[i].native == buffer) {
            *index = i;
            return VK_SUCCESS;
        }
    hybris_vk_wayland_window_cancel(state->window, buffer, -1);
    if (*fence >= 0) close(*fence);
    *fence = -1;
    return VK_ERROR_UNKNOWN;
}

static VkResult VKAPI_CALL acquire_next_image(VkDevice device, VkSwapchainKHR swapchain,
    uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    struct hybris_icd_device context;
    if (!index || !hybris_icd_lookup_device(device, &context) || !context.swapchain_enabled)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    struct swapchain_state *state = find_swapchain(swapchain);
    if (!state || state->device != device || state->retired)
        return VK_ERROR_OUT_OF_DATE_KHR;
    int native_fence = -1;
    VkResult result = acquire_slot(state, dequeue_timeout(timeout), index, &native_fence);
    if (result != VK_SUCCESS) return result;
    PFN_vkAcquireImageANDROID acquire = (PFN_vkAcquireImageANDROID)
        context.resolver(device, "vkAcquireImageANDROID");
    if (!acquire) {
        if (native_fence >= 0) close(native_fence);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    /* Driver owns native_fence after this call, including failure. */
    result = acquire(device, state->images[*index].image, native_fence, semaphore, fence);
    if (result != VK_SUCCESS) return result;
    state->images[*index].state = IMAGE_ACQUIRED;
    return VK_SUCCESS;
}

static VkResult present_one(struct hybris_icd_device *context, VkQueue queue,
    const VkPresentInfoKHR *info, uint32_t entry)
{
    struct swapchain_state *state = find_swapchain(info->pSwapchains[entry]);
    uint32_t index = info->pImageIndices[entry];
    if (!state || state->device != context->handle || state->retired)
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (index >= state->count || state->images[index].state != IMAGE_ACQUIRED)
        return VK_ERROR_UNKNOWN;
    PFN_vkQueueSignalReleaseImageANDROID release_image =
        (PFN_vkQueueSignalReleaseImageANDROID)context->resolver(context->handle,
            "vkQueueSignalReleaseImageANDROID");
    if (!release_image) return VK_ERROR_EXTENSION_NOT_PRESENT;
    int fence = -1;
    VkResult result = release_image(queue, info->waitSemaphoreCount, info->pWaitSemaphores,
        state->images[index].image, &fence);
    if (result != VK_SUCCESS) {
        if (fence >= 0) close(fence);
        return result;
    }
    int error = hybris_vk_wayland_window_queue(state->window, state->images[index].native, fence);
    if (error) {
        /* Native queueBuffer closes fenceFd after taking it. Only unused
         * fences from the EINVAL wrapper path remain ours. */
        if (error == -EINVAL && fence >= 0) close(fence);
        return VK_ERROR_UNKNOWN;
    }
    state->images[index].state = IMAGE_PRESENTED;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    if (!info || !info->pSwapchains || !info->pImageIndices)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        struct swapchain_state *state = find_swapchain(info->pSwapchains[i]);
        struct hybris_icd_device context;
        VkResult result = VK_ERROR_UNKNOWN;
        if (state && hybris_icd_lookup_device(state->device, &context))
            result = present_one(&context, queue, info, i);
        if (info->pResults) info->pResults[i] = result;
        if (result != VK_SUCCESS) worst = result;
    }
    return worst;
}

void hybris_icd_swapchain_release_device(VkDevice device)
{
    struct swapchain_state *orphans = NULL;
    pthread_mutex_lock(&swapchain_guard);
    struct swapchain_state **link = &swapchains;
    while (*link) {
        if ((*link)->device == device) {
            struct swapchain_state *state = *link;
            *link = state->next;
            state->next = orphans;
            orphans = state;
        } else {
            link = &(*link)->next;
        }
    }
    pthread_mutex_unlock(&swapchain_guard);
    struct hybris_icd_device context;
    int found = hybris_icd_lookup_device(device, &context);
    while (orphans) {
        struct swapchain_state *next = orphans->next;
        destroy_images(found ? &context : NULL, orphans);
        object_free(&orphans->allocator, orphans->custom_allocator, orphans->images);
        object_free(&orphans->allocator, orphans->custom_allocator, orphans);
        orphans = next;
    }
}
#else
void hybris_icd_swapchain_release_device(VkDevice device)
{
    (void)device;
}
#endif

VkResult hybris_icd_prepare_device(VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr resolver,
    VkInstance instance, const VkDeviceCreateInfo *info, VkDeviceCreateInfo *filtered,
    const char ***names, int *swapchain_enabled)
{
    *filtered = *info;
    *names = NULL;
    *swapchain_enabled = 0;
#ifdef WANT_WAYLAND
    uint32_t count = info->enabledExtensionCount;
    const char **kept = count || 1 ? malloc((count + 1) * sizeof(*kept)) : NULL;
    if (!kept) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t kept_count = 0;
    int anb = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = info->ppEnabledExtensionNames[i];
        if (!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            *swapchain_enabled = 1;
            continue;
        }
        if (!strcmp(name, VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME)) anb = 1;
        kept[kept_count++] = name;
    }
    if (*swapchain_enabled && !hybris_icd_physical_has_native_buffer(physical)) {
        free(kept);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (*swapchain_enabled && !anb)
        kept[kept_count++] = VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME;
    *names = kept;
    filtered->enabledExtensionCount = kept_count;
    filtered->ppEnabledExtensionNames = kept_count ? kept : NULL;
#else
    (void)physical;
    (void)resolver;
    (void)instance;
#endif
    return VK_SUCCESS;
}

void hybris_icd_finish_device(const char **names)
{
    free((void *)names);
}

VkResult hybris_icd_enumerate_device_extensions(VkPhysicalDevice physical, const char *layer,
    uint32_t *count, VkExtensionProperties *properties)
{
    struct hybris_icd_physical context;
    if (!count || !hybris_icd_lookup_physical(physical, &context))
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
        context.resolver(context.instance, "vkEnumerateDeviceExtensionProperties");
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return enumerate(physical, layer, count, properties);
    uint32_t hal_count = 0;
    VkResult result = enumerate(physical, NULL, &hal_count, NULL);
    if (result != VK_SUCCESS) return result;
#ifdef WANT_WAYLAND
    int extra = hybris_icd_physical_has_native_buffer(physical);
    static const VkExtensionProperties swapchain = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION};
#else
    const int extra = 0;
#endif
    uint32_t total = hal_count + (extra ? 1u : 0u);
    if (!properties) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t space = *count;
    uint32_t written = 0;
    if (hal_count) {
        VkExtensionProperties *hal_props = calloc(hal_count, sizeof(*hal_props));
        if (!hal_props) return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t query = hal_count;
        result = enumerate(physical, NULL, &query, hal_props);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            free(hal_props);
            return result;
        }
        written = space < query ? space : query;
        if (written) memcpy(properties, hal_props, written * sizeof(*properties));
        free(hal_props);
    }
#ifdef WANT_WAYLAND
    if (extra && written < space) properties[written++] = swapchain;
#endif
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

PFN_vkVoidFunction hybris_icd_swapchain_proc(const char *name, int swapchain_enabled)
{
#ifdef WANT_WAYLAND
    if (!swapchain_enabled) return NULL;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)create_swapchain;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)destroy_swapchain;
    if (!strcmp(name, "vkGetSwapchainImagesKHR")) return (PFN_vkVoidFunction)get_swapchain_images;
    if (!strcmp(name, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction)acquire_next_image;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)queue_present;
#endif
    (void)name;
    (void)swapchain_enabled;
    return NULL;
}
