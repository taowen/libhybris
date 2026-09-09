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
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef WANT_WAYLAND
#include <hybris/gralloc/gralloc.h>
#include <hybris/grallocusage/GrallocUsageConversion.h>
#include <system/graphics.h>
#include <system/window.h>
#include "native_window.h"
#endif

#ifdef WANT_WAYLAND
enum image_state { IMAGE_FREE, IMAGE_ACQUIRED, IMAGE_PRESENTED };

struct swapchain_image {
    VkImage image;
    struct ANativeWindowBuffer *native;
    enum image_state state;
    int acquire_fence;
    VkSemaphore present_ready;
};

struct swapchain_state {
    VkDevice device;
    uint64_t device_generation;
    VkSurfaceKHR surface;
    struct hybris_icd_window *window;
    uint32_t count;
    VkExtent2D extent;
    VkResult presentation_status;
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
    struct hybris_icd_window *window = NULL;
    pthread_mutex_lock(&swapchain_guard);
    struct swapchain_state *state = swapchains;
    while (state && (VkSwapchainKHR)(uintptr_t)state != swapchain)
        state = state->next;
    if (state) {
        state->retired = 1;
        window = state->window;
    }
    pthread_mutex_unlock(&swapchain_guard);
    if (window) window->ops->disconnect(window);
}

static int hal_format(VkFormat format)
{
    if (format == VK_FORMAT_R8G8B8A8_UNORM) return HAL_PIXEL_FORMAT_RGBA_8888;
    if (format == VK_FORMAT_B8G8R8A8_UNORM) return HAL_PIXEL_FORMAT_BGRA_8888;
    return 0;
}

static VkResult import_image(const struct hybris_icd_device *device,
    const VkSwapchainCreateInfoKHR *info, struct ANativeWindowBuffer *native,
    int usage, const VkAllocationCallbacks *allocator, VkImage *image)
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
    return create(device->handle, &image_info, allocator, image);
}

static void destroy_images(const struct hybris_icd_device *device, struct swapchain_state *state)
{
    PFN_vkDestroyImage destroy = device
        ? (PFN_vkDestroyImage)device->resolver(device->handle, "vkDestroyImage") : NULL;
    for (uint32_t i = 0; i < state->count; ++i) {
        if (state->images[i].present_ready && device) {
            PFN_vkDestroySemaphore destroy_semaphore = (PFN_vkDestroySemaphore)
                device->resolver(device->handle, "vkDestroySemaphore");
            destroy_semaphore(device->handle, state->images[i].present_ready,
                state->custom_allocator ? &state->allocator : NULL);
        }
        if (state->images[i].image && destroy)
            destroy(state->device, state->images[i].image, state->custom_allocator ? &state->allocator : NULL);
        if (state->images[i].native && !state->retired && state->window &&
            state->images[i].state != IMAGE_PRESENTED) {
            state->window->ops->cancel(state->window, state->images[i].native,
                state->images[i].acquire_fence);
        } else if (state->images[i].acquire_fence >= 0) {
            close(state->images[i].acquire_fence);
        }
        if (state->images[i].native)
            state->images[i].native->common.decRef(&state->images[i].native->common);
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
    // Retirement occurs on the creation attempt, even if allocation fails.
    if (info->oldSwapchain) retire_swapchain(info->oldSwapchain);
    if (!info->imageExtent.width || !info->imageExtent.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    int pixel = hal_format(info->imageFormat);
    if (!pixel) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* RGBX preserves the Vulkan RGBA image layout while making the native
     * compositor ignore application alpha, as OPAQUE requires. */
    if (info->compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR &&
        pixel == HAL_PIXEL_FORMAT_RGBA_8888)
        pixel = HAL_PIXEL_FORMAT_RGBX_8888;
    VkInstance instance = VK_NULL_HANDLE;
    uint64_t generation = 0;
    struct hybris_icd_window *window =
        hybris_icd_wsi_surface_window(info->surface, &instance, &generation);
    if (!window || generation != context.instance_generation)
        return VK_ERROR_SURFACE_LOST_KHR;
    int in_use = 0;
    pthread_mutex_lock(&swapchain_guard);
    for (struct swapchain_state *active = swapchains; active; active = active->next)
        if (active->surface == info->surface && !active->retired) in_use = 1;
    pthread_mutex_unlock(&swapchain_guard);
    if (in_use) return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    if (info->flags || info->imageArrayLayers != 1 ||
        info->presentMode != VK_PRESENT_MODE_FIFO_KHR ||
        info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceCapabilitiesKHR capabilities;
    VkResult checked = hybris_icd_wsi_capabilities(context.physical, info->surface, &capabilities);
    if (checked != VK_SUCCESS) return checked;
    if (capabilities.currentExtent.width != UINT32_MAX &&
        (info->imageExtent.width != capabilities.currentExtent.width ||
         info->imageExtent.height != capabilities.currentExtent.height))
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (info->imageExtent.width > capabilities.maxImageExtent.width ||
        info->imageExtent.height > capabilities.maxImageExtent.height ||
        (info->imageUsage & ~capabilities.supportedUsageFlags) ||
        !(info->compositeAlpha & capabilities.supportedCompositeAlpha))
        return VK_ERROR_INITIALIZATION_FAILED;
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR &&
            ((const VkDeviceGroupSwapchainCreateInfoKHR *)next)->modes != VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_vkGetSwapchainGrallocUsage2ANDROID usage2 =
        (PFN_vkGetSwapchainGrallocUsage2ANDROID)context.resolver(device,
            "vkGetSwapchainGrallocUsage2ANDROID");
    if (!usage2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint64_t consumer = 0, producer = 0;
    VkResult result = usage2(device, info->imageFormat, info->imageUsage, 0, &consumer, &producer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "hybris WSI: gralloc usage query failed: result=%d format=%d usage=0x%x\n",
            result, info->imageFormat, info->imageUsage);
        return result;
    }
    if ((producer | consumer) >> 32) {
        fprintf(stderr, "hybris WSI: gralloc usage exceeds legacy transport: producer=0x%" PRIx64
            " consumer=0x%" PRIx64 " format=%d usage=0x%x\n",
            producer, consumer, info->imageFormat, info->imageUsage);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    int usage = android_convertGralloc1To0Usage(producer, consumer);
    uint32_t count = info->minImageCount;
    if (count < 2 || count > 8) return VK_ERROR_INITIALIZATION_FAILED;
    struct swapchain_state *state = object_alloc(allocator, sizeof(*state),
        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(state, 0, sizeof(*state));
    state->device = device;
    state->device_generation = context.generation;
    state->surface = info->surface;
    state->window = window;
    state->extent = info->imageExtent;
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
    for (uint32_t i = 0; i < count; ++i) state->images[i].acquire_fence = -1;
    // Begin a fresh pool, including after destruction without oldSwapchain.
    window->ops->disconnect(window);
    if (window->ops->configure(window, info->imageExtent.width, info->imageExtent.height,
            pixel, (unsigned)usage, count)) {
        destroy_images(&context, state);
        window->ops->disconnect(window);
        object_free(allocator, state->custom_allocator, state->images);
        object_free(allocator, state->custom_allocator, state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i) {
        struct ANativeWindowBuffer *buffer = NULL;
        int fence = -1;
        int error = window->ops->dequeue(window, 5000000000LL, &buffer, &fence);
        if (error || !buffer) {
            if (fence >= 0) close(fence);
            destroy_images(&context, state);
            object_free(allocator, state->custom_allocator, state->images);
            object_free(allocator, state->custom_allocator, state);
            return error == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN;
        }
        // Keep the native backing alive beyond producer-pool retirement, and
        // preserve its acquire fence until the first driver acquisition.
        buffer->common.incRef(&buffer->common);
        state->images[i].native = buffer;
        state->images[i].acquire_fence = fence;
        result = import_image(&context, info, buffer, buffer->usage, allocator,
            &state->images[i].image);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "hybris WSI: native image import failed: result=%d format=%d usage=0x%x"
                " native_format=%d native_usage=0x%x extent=%ux%u\n",
                result, info->imageFormat, info->imageUsage, buffer->format,
                (unsigned)buffer->usage, info->imageExtent.width, info->imageExtent.height);
            destroy_images(&context, state);
            object_free(allocator, state->custom_allocator, state->images);
            object_free(allocator, state->custom_allocator, state);
            return result;
        }
        PFN_vkCreateSemaphore create_semaphore = (PFN_vkCreateSemaphore)
            context.resolver(device, "vkCreateSemaphore");
        VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = create_semaphore(device, &semaphore, allocator, &state->images[i].present_ready);
        if (result != VK_SUCCESS) {
            destroy_images(&context, state);
            object_free(allocator, state->custom_allocator, state->images);
            object_free(allocator, state->custom_allocator, state);
            return result;
        }
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
    if (!state->retired) state->window->ops->disconnect(state->window);
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

/* A size mismatch is sticky until the application replaces the chain. The
 * Wayland owner reports zero extent because its size is application selected.
 * Keep this separate from retirement: already acquired retired images may
 * still be presented if the native surface remains compatible. */
static VkResult presentation_status(struct swapchain_state *state)
{
    if (state->presentation_status != VK_SUCCESS) return state->presentation_status;
    uint32_t width = 0, height = 0;
    if (state->window->ops->extent(state->window, &width, &height))
        state->presentation_status = VK_ERROR_SURFACE_LOST_KHR;
    else if ((width && width != state->extent.width) ||
             (height && height != state->extent.height))
        /* The native pool still contains valid presentable images. Report
         * the size mismatch without retiring the swapchain; the application
         * can finish this frame and choose when to rebuild the pool. */
        return VK_SUBOPTIMAL_KHR;
    return state->presentation_status;
}

static VkResult acquire_slot(struct swapchain_state *state, int64_t timeout_ns, uint32_t *index,
    int *fence)
{
    *fence = -1;
    for (uint32_t i = 0; i < state->count; ++i)
        if (state->images[i].state == IMAGE_FREE) {
            *index = i;
            *fence = state->images[i].acquire_fence;
            state->images[i].acquire_fence = -1;
            return VK_SUCCESS;
        }
    struct ANativeWindowBuffer *buffer = NULL;
    int error = state->window->ops->dequeue(state->window, timeout_ns, &buffer, fence);
    if (error == -ESTALE) {
        state->presentation_status = VK_ERROR_OUT_OF_DATE_KHR;
        return state->presentation_status;
    }
    if (error == -EAGAIN) return VK_NOT_READY;
    if (error == -ETIMEDOUT) return VK_TIMEOUT;
    if (error || !buffer) {
        if (*fence >= 0) { close(*fence); *fence = -1; }
        return VK_ERROR_UNKNOWN;
    }
    for (uint32_t i = 0; i < state->count; ++i)
        if (state->images[i].native == buffer) {
            state->images[i].state = IMAGE_FREE;
            *index = i;
            return VK_SUCCESS;
        }
    state->window->ops->cancel(state->window, buffer, *fence);
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
    VkResult status = presentation_status(state);
    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) return status;
    VkResult result;
    int native_fence = -1;
    uint32_t slot = 0;
    result = acquire_slot(state, dequeue_timeout(timeout), &slot, &native_fence);
    if (result != VK_SUCCESS) return result;
    PFN_vkAcquireImageANDROID acquire = (PFN_vkAcquireImageANDROID)
        context.resolver(device, "vkAcquireImageANDROID");
    if (!acquire) {
        if (native_fence >= 0) close(native_fence);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    /* Driver owns native_fence after this call, including failure. */
    result = acquire(device, state->images[slot].image, native_fence, semaphore, fence);
    if (result != VK_SUCCESS) return result;
    state->images[slot].state = IMAGE_ACQUIRED;
    *index = slot;
    return status;
}

static VkResult VKAPI_CALL acquire_next_image2(VkDevice device,
    const VkAcquireNextImageInfoKHR *info, uint32_t *index)
{
    if (!info || info->deviceMask != 1) return VK_ERROR_INITIALIZATION_FAILED;
    return acquire_next_image(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

static VkResult VKAPI_CALL group_capabilities(VkDevice device,
    VkDeviceGroupPresentCapabilitiesKHR *capabilities)
{
    struct hybris_icd_device context;
    if (!hybris_icd_lookup_device(device, &context) || !context.swapchain_enabled)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    memset(capabilities->presentMask, 0, sizeof(capabilities->presentMask));
    capabilities->presentMask[0] = 1;
    capabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL group_surface_modes(VkDevice device, VkSurfaceKHR surface,
    VkDeviceGroupPresentModeFlagsKHR *modes)
{
    struct hybris_icd_device context;
    if (!hybris_icd_lookup_device(device, &context) || !context.swapchain_enabled)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkSurfaceCapabilitiesKHR capabilities;
    VkResult result = hybris_icd_wsi_capabilities(context.physical, surface, &capabilities);
    if (result != VK_SUCCESS) return result;
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

/* Core 1.1 image-alias chains carry adapter-owned swapchain handles. Until
 * backing-memory alias binding is implemented, reject that combination here;
 * forwarding it to an Android HAL would hand it a foreign pointer. */
static VkResult VKAPI_CALL create_image(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *image)
{
    struct hybris_icd_device context;
    if (!hybris_icd_lookup_device(device, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR &&
            ((const VkImageSwapchainCreateInfoKHR *)next)->swapchain != VK_NULL_HANDLE)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    PFN_vkCreateImage create = (PFN_vkCreateImage)context.resolver(device, "vkCreateImage");
    return create(device, info, allocator, image);
}

static VkResult bind_images(VkDevice device, uint32_t count,
    const VkBindImageMemoryInfo *infos, const char *name)
{
    struct hybris_icd_device context;
    if (!hybris_icd_lookup_device(device, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; ++i)
        for (const VkBaseInStructure *next = infos[i].pNext; next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR &&
                ((const VkBindImageMemorySwapchainInfoKHR *)next)->swapchain != VK_NULL_HANDLE)
                return VK_ERROR_UNKNOWN;
    PFN_vkBindImageMemory2 bind = (PFN_vkBindImageMemory2)context.resolver(device, name);
    return bind ? bind(device, count, infos) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult VKAPI_CALL bind_images_core(VkDevice device, uint32_t count,
    const VkBindImageMemoryInfo *infos)
{
    return bind_images(device, count, infos, "vkBindImageMemory2");
}

static VkResult VKAPI_CALL bind_images_khr(VkDevice device, uint32_t count,
    const VkBindImageMemoryInfo *infos)
{
    return bind_images(device, count, infos, "vkBindImageMemory2KHR");
}

static VkResult present_one(struct hybris_icd_device *context, VkQueue queue,
    const VkPresentInfoKHR *info, uint32_t entry)
{
    struct swapchain_state *state = find_swapchain(info->pSwapchains[entry]);
    uint32_t index = info->pImageIndices[entry];
    if (!state || state->device != context->handle || state->device_generation != context->generation)
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
    /* A resized surface can still consume the old pool's image. Fatal
     * statuses cancel it after consuming the application's semaphore waits. */
    result = presentation_status(state);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        state->window->ops->cancel(state->window, state->images[index].native, fence);
        state->images[index].state = IMAGE_FREE;
        return result;
    }
    int error = state->window->ops->queue(state->window, state->images[index].native, fence);
    // The native queue wrapper consumes the FD on every return path.
    if (error) return VK_ERROR_SURFACE_LOST_KHR;
    state->images[index].state = IMAGE_PRESENTED;
    return result;
}

static VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    if (!info || (info->swapchainCount && (!info->pSwapchains || !info->pImageIndices)))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct hybris_icd_device context;
    if (!hybris_icd_lookup_queue(queue, &context) || !context.swapchain_enabled)
        return VK_ERROR_UNKNOWN;
    VkSemaphore *signals = NULL;
    // Each Android release must get an explicit dependency: with zero waits
    // a driver may return an already-signaled FD without inspecting the image.
    // Fan out one application wait into one semaphore per presented image.
    if (info->swapchainCount != 1 && info->waitSemaphoreCount) {
        VkPipelineStageFlags *stages = malloc(info->waitSemaphoreCount * sizeof(*stages));
        signals = info->swapchainCount ? malloc(info->swapchainCount * sizeof(*signals)) : NULL;
        if (!stages || (info->swapchainCount && !signals)) {
            free(stages); free(signals);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
            stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        for (uint32_t i = 0; i < info->swapchainCount; ++i) {
            struct swapchain_state *state = find_swapchain(info->pSwapchains[i]);
            uint32_t index = info->pImageIndices[i];
            if (!state || state->device != context.handle || index >= state->count ||
                state->images[index].state != IMAGE_ACQUIRED) {
                free(stages); free(signals);
                return VK_ERROR_UNKNOWN;
            }
            signals[i] = state->images[index].present_ready;
        }
        VkSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = info->waitSemaphoreCount,
            .pWaitSemaphores = info->pWaitSemaphores, .pWaitDstStageMask = stages,
            .signalSemaphoreCount = info->swapchainCount, .pSignalSemaphores = signals};
        PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)context.resolver(context.handle, "vkQueueSubmit");
        VkResult result = submit(queue, 1, &wait, VK_NULL_HANDLE);
        free(stages);
        if (result != VK_SUCCESS) {
            free(signals);
            if (info->pResults)
                for (uint32_t i = 0; i < info->swapchainCount; ++i) info->pResults[i] = result;
            return result;
        }
    }
    VkPresentInfoKHR ready = *info;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        if (signals) {
            ready.waitSemaphoreCount = 1;
            ready.pWaitSemaphores = &signals[i];
        }
        VkResult result = present_one(&context, queue, &ready, i);
        if (info->pResults) info->pResults[i] = result;
        if (worst == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST ||
            (result == VK_ERROR_SURFACE_LOST_KHR && worst != VK_ERROR_DEVICE_LOST) ||
            (result < 0 && worst == VK_SUBOPTIMAL_KHR))
            worst = result;
    }
    free(signals);
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
    const char **kept = malloc(((size_t)count + 1) * sizeof(*kept));
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
    if (*swapchain_enabled) {
        for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
                ((const VkDeviceGroupDeviceCreateInfo *)next)->physicalDeviceCount != 1) {
                free(kept);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        if (!anb) kept[kept_count++] = VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME;
    }
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
    if (!strcmp(name, "vkAcquireNextImage2KHR")) return (PFN_vkVoidFunction)acquire_next_image2;
    if (!strcmp(name, "vkGetDeviceGroupPresentCapabilitiesKHR")) return (PFN_vkVoidFunction)group_capabilities;
    if (!strcmp(name, "vkGetDeviceGroupSurfacePresentModesKHR")) return (PFN_vkVoidFunction)group_surface_modes;
#endif
    (void)name;
    (void)swapchain_enabled;
    return NULL;
}

PFN_vkVoidFunction hybris_icd_swapchain_image_proc(const char *name)
{
#ifdef WANT_WAYLAND
    if (!strcmp(name, "vkCreateImage")) return (PFN_vkVoidFunction)create_image;
    if (!strcmp(name, "vkBindImageMemory2")) return (PFN_vkVoidFunction)bind_images_core;
    if (!strcmp(name, "vkBindImageMemory2KHR")) return (PFN_vkVoidFunction)bind_images_khr;
#endif
    (void)name;
    return NULL;
}
