/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "swapchain_review.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Application callbacks audit the actual driver's imported-image allocations,
 * including recovery from an intentionally rejected swapchain allocation. */
struct allocation { void *pointer; size_t size; struct allocation *next; };
struct allocator_state {
    pthread_mutex_t guard;
    struct allocation *head;
    unsigned reject, failed;
};
static void *VKAPI_PTR allocate(void *data, size_t size, size_t alignment, VkSystemAllocationScope scope)
{
    (void)scope;
    struct allocator_state *state = data;
    pthread_mutex_lock(&state->guard);
    int reject = state->reject != 0;
    if (reject) --state->reject;
    pthread_mutex_unlock(&state->guard);
    if (reject) return NULL;
    struct allocation *entry = malloc(sizeof(*entry));
    if (!entry) return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (posix_memalign(&entry->pointer, alignment, size ? size : 1)) { free(entry); return NULL; }
    entry->size = size;
    pthread_mutex_lock(&state->guard);
    entry->next = state->head;
    state->head = entry;
    pthread_mutex_unlock(&state->guard);
    return entry->pointer;
}
static void VKAPI_PTR release(void *data, void *pointer)
{
    if (!pointer) return;
    struct allocator_state *state = data;
    pthread_mutex_lock(&state->guard);
    struct allocation **link = &state->head;
    while (*link && (*link)->pointer != pointer) link = &(*link)->next;
    struct allocation *entry = *link;
    if (entry) *link = entry->next;
    else state->failed = 1;
    pthread_mutex_unlock(&state->guard);
    if (entry) { free(entry->pointer); free(entry); }
}
static void *VKAPI_PTR reallocate(void *data, void *pointer, size_t size,
    size_t alignment, VkSystemAllocationScope scope)
{
    if (!pointer) return allocate(data, size, alignment, scope);
    if (!size) { release(data, pointer); return NULL; }
    struct allocator_state *state = data;
    size_t previous = 0;
    pthread_mutex_lock(&state->guard);
    struct allocation *entry = state->head;
    while (entry && entry->pointer != pointer) entry = entry->next;
    if (entry) previous = entry->size;
    else state->failed = 1;
    pthread_mutex_unlock(&state->guard);
    if (!entry) return NULL;
    void *replacement = allocate(data, size, alignment, scope);
    if (replacement) {
        memcpy(replacement, pointer, previous < size ? previous : size);
        release(data, pointer);
    }
    return replacement;
}
#define LOAD(name) PFN_##name name = (PFN_##name)resolver(instance, #name); \
    if (!name) { printf("WSI_REVIEW missing %s\n", #name); return 2; }
#define OK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    printf("WSI_REVIEW %s = %d\n", #call, r); return 2; } } while (0)
static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

int swapchain_review(PFN_vkGetInstanceProcAddr resolver, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
    const VkSwapchainCreateInfoKHR *model)
{
    LOAD(vkCreateSwapchainKHR); LOAD(vkDestroySwapchainKHR); LOAD(vkGetSwapchainImagesKHR);
    LOAD(vkAcquireNextImageKHR); LOAD(vkAcquireNextImage2KHR); LOAD(vkQueuePresentKHR);
    LOAD(vkGetDeviceGroupPresentCapabilitiesKHR); LOAD(vkGetDeviceGroupSurfacePresentModesKHR);
    LOAD(vkGetPhysicalDevicePresentRectanglesKHR);
    LOAD(vkCreateFence); LOAD(vkDestroyFence); LOAD(vkWaitForFences); LOAD(vkResetFences); LOAD(vkGetFenceStatus);
    LOAD(vkCreateSemaphore); LOAD(vkDestroySemaphore); LOAD(vkQueueSubmit); LOAD(vkQueueWaitIdle);
    LOAD(vkCreateCommandPool); LOAD(vkDestroyCommandPool); LOAD(vkAllocateCommandBuffers); LOAD(vkResetCommandPool);
    LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkCmdPipelineBarrier); LOAD(vkCmdClearColorImage);
    LOAD(vkCreateBuffer); LOAD(vkDestroyBuffer); LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkGetPhysicalDeviceMemoryProperties); LOAD(vkAllocateMemory); LOAD(vkFreeMemory);
    LOAD(vkBindBufferMemory); LOAD(vkCmdCopyImageToBuffer); LOAD(vkMapMemory); LOAD(vkUnmapMemory);
    struct allocator_state allocations = {.guard = PTHREAD_MUTEX_INITIALIZER};
    VkAllocationCallbacks allocator = {.pUserData = &allocations,
        .pfnAllocation = allocate, .pfnReallocation = reallocate, .pfnFree = release};
    VkSwapchainCreateInfoKHR create = *model;
    VkSwapchainKHR old, fresh;
    OK(vkCreateSwapchainKHR(device, &create, &allocator, &old));
    uint32_t count = 0;
    OK(vkGetSwapchainImagesKHR(device, old, &count, NULL));
    if (!count || count > 8) return 2;
    VkImage old_images[8];
    OK(vkGetSwapchainImagesKHR(device, old, &count, old_images));
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acquired;
    OK(vkCreateFence(device, &fc, NULL, &acquired));
    uint32_t indices[8], mask = 0;
    for (uint32_t i = 0; i < count; ++i) {
        OK(vkAcquireNextImageKHR(device, old, 5000000000ull, VK_NULL_HANDLE, acquired, &indices[i]));
        if (indices[i] >= count || (mask & (1u << indices[i]))) return 2;
        mask |= 1u << indices[i];
        OK(vkWaitForFences(device, 1, &acquired, VK_TRUE, 5000000000ull));
        OK(vkResetFences(device, 1, &acquired));
    }
    uint32_t unavailable = UINT32_MAX;
    VkResult result = vkAcquireNextImageKHR(device, old, 0, VK_NULL_HANDLE, acquired, &unavailable);
    if (result != VK_NOT_READY || unavailable != UINT32_MAX || vkGetFenceStatus(device, acquired) != VK_NOT_READY) return 2;
    uint64_t started = now_ns();
    result = vkAcquireNextImageKHR(device, old, 20000000ull, VK_NULL_HANDLE, acquired, &unavailable);
    uint64_t elapsed = now_ns() - started;
    printf("WSI_REVIEW exhausted zero=NOT_READY finite=%d elapsed_ns=%llu\n", result, (unsigned long long)elapsed);
    if (result != VK_TIMEOUT || elapsed < 15000000ull || elapsed > 2000000000ull ||
        unavailable != UINT32_MAX || vkGetFenceStatus(device, acquired) != VK_NOT_READY) return 2;
    allocations.reject = 1;
    create.oldSwapchain = old;
    result = vkCreateSwapchainKHR(device, &create, &allocator, &fresh);
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY || allocations.reject) return 2;
    // Failed replacement retires old. Its acquired images remain usable.
    // A new chain without oldSwapchain is now legal on the same surface.
    create.oldSwapchain = VK_NULL_HANDLE;
    OK(vkCreateSwapchainKHR(device, &create, &allocator, &fresh));
    uint32_t fresh_count = 8, fresh_index;
    VkImage fresh_images[8];
    OK(vkGetSwapchainImagesKHR(device, fresh, &fresh_count, fresh_images));
    VkDeviceGroupPresentCapabilitiesKHR group = {.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_CAPABILITIES_KHR};
    OK(vkGetDeviceGroupPresentCapabilitiesKHR(device, &group));
    if (group.presentMask[0] != 1 || group.modes != VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR) return 2;
    for (unsigned i = 1; i < VK_MAX_DEVICE_GROUP_SIZE; ++i) if (group.presentMask[i]) return 2;
    VkDeviceGroupPresentModeFlagsKHR modes;
    OK(vkGetDeviceGroupSurfacePresentModesKHR(device, create.surface, &modes));
    if (modes != VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR) return 2;
    uint32_t rect_count = 1;
    VkRect2D rectangle;
    OK(vkGetPhysicalDevicePresentRectanglesKHR(physical, create.surface, &rect_count, &rectangle));
    if (rect_count != 1 || rectangle.extent.width < create.imageExtent.width ||
        rectangle.extent.height < create.imageExtent.height) return 2;
    VkAcquireNextImageInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
        .swapchain = fresh, .timeout = 5000000000ull, .fence = acquired, .deviceMask = 1};
    OK(vkAcquireNextImage2KHR(device, &ai, &fresh_index));
    if (fresh_index >= fresh_count) return 2;
    OK(vkWaitForFences(device, 1, &acquired, VK_TRUE, 5000000000ull));
    OK(vkResetFences(device, 1, &acquired));
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool;
    OK(vkCreateCommandPool(device, &pc, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    OK(vkAllocateCommandBuffers(device, &ca, &command));
    VkDeviceSize image_size = (VkDeviceSize)create.imageExtent.width * create.imageExtent.height * 4;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = image_size * 2, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    OK(vkCreateBuffer(device, &bc, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { memory_type = i; break; }
    if (memory_type == UINT32_MAX) return 2;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = memory_type};
    VkDeviceMemory memory;
    OK(vkAllocateMemory(device, &ma, NULL, &memory));
    OK(vkBindBufferMemory(device, buffer, memory, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    OK(vkBeginCommandBuffer(command, &begin));
    VkImage targets[] = {old_images[indices[0]], fresh_images[fresh_index]};
    for (unsigned i = 0; i < 2; ++i) {
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = targets[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkClearColorValue color = {.float32 = {i ? 1 : 0, i ? 1 : 0, i ? 0 : 1, 1}};
        vkCmdClearColorImage(command, targets[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkBufferImageCopy copy = {.bufferOffset = image_size * i,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {create.imageExtent.width, create.imageExtent.height, 1}};
        vkCmdCopyImageToBuffer(command, targets[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    }
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    OK(vkEndCommandBuffer(command));
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore ready;
    OK(vkCreateSemaphore(device, &sem, NULL, &ready));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
        .pCommandBuffers = &command, .signalSemaphoreCount = 1, .pSignalSemaphores = &ready};
    OK(vkQueueSubmit(queue, 1, &submit, acquired));
    VkSwapchainKHR chains[] = {old, fresh};
    uint32_t image_indices[] = {indices[0], fresh_index};
    VkResult results[2] = {VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN};
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &ready, .swapchainCount = 2,
        .pSwapchains = chains, .pImageIndices = image_indices, .pResults = results};
    OK(vkQueuePresentKHR(queue, &present));
    if (results[0] != VK_SUCCESS || results[1] != VK_SUCCESS) return 2;
    OK(vkWaitForFences(device, 1, &acquired, VK_TRUE, 5000000000ull));
    unsigned char *pixels;
    OK(vkMapMemory(device, memory, 0, image_size * 2, 0, (void **)&pixels));
    for (unsigned image = 0; image < 2; ++image)
        for (VkDeviceSize pixel = 0; pixel < image_size; pixel += 4) {
            unsigned char expected[4] = {image ? 255 : 0, image ? 255 : 0, image ? 0 : 255, 255};
            if (create.imageFormat == VK_FORMAT_B8G8R8A8_UNORM) {
                unsigned char red = expected[0]; expected[0] = expected[2]; expected[2] = red;
            }
            if (memcmp(pixels + image * image_size + pixel, expected, 4)) return 2;
        }
    vkUnmapMemory(device, memory);
    OK(vkQueueWaitIdle(queue));
    // Hold every other fresh image, then present one of them. Reacquisition
    // must read the previous buffer's wl_buffer.release from the socket;
    // there is deliberately no application Wayland dispatch between calls.
    OK(vkResetFences(device, 1, &acquired));
    uint32_t held[8];
    for (uint32_t i = 0; i + 1 < fresh_count; ++i) {
        OK(vkAcquireNextImageKHR(device, fresh, 5000000000ull, VK_NULL_HANDLE, acquired, &held[i]));
        if (held[i] == fresh_index || held[i] >= fresh_count) return 2;
        OK(vkWaitForFences(device, 1, &acquired, VK_TRUE, 5000000000ull));
        OK(vkResetFences(device, 1, &acquired));
    }
    OK(vkResetCommandPool(device, pool, 0));
    OK(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier change = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = fresh_images[held[0]],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &change);
    VkClearColorValue magenta = {.float32 = {1, 0, 1, 1}};
    vkCmdClearColorImage(command, change.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &magenta, 1, &change.subresourceRange);
    change.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; change.dstAccessMask = 0;
    change.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; change.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &change);
    OK(vkEndCommandBuffer(command));
    OK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    present.swapchainCount = 1; present.pSwapchains = &fresh; present.pImageIndices = &held[0];
    OK(vkQueuePresentKHR(queue, &present));
    uint32_t released;
    OK(vkAcquireNextImageKHR(device, fresh, 5000000000ull, VK_NULL_HANDLE, acquired, &released));
    if (released != fresh_index) return 2;
    OK(vkWaitForFences(device, 1, &acquired, VK_TRUE, 5000000000ull));
    OK(vkQueueWaitIdle(queue));
    printf("WSI_REVIEW release_socket=PASS reacquired=%u\n", released);
    vkDestroySwapchainKHR(device, old, &allocator);
    vkDestroySwapchainKHR(device, fresh, &allocator);
    vkDestroySemaphore(device, ready, NULL);
    vkDestroyFence(device, acquired, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    if (allocations.head || allocations.failed) {
        printf("WSI_REVIEW allocator unfreed=%d foreign_free=%u\n", allocations.head != NULL, allocations.failed);
        return 2;
    }
    pthread_mutex_destroy(&allocations.guard);
    printf("WSI_REVIEW PASS old-and-new-images=readback multi-present=2 one-wait=1 allocator=balanced\n");
    return 0;
}
