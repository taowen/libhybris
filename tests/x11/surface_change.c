/* SPDX-License-Identifier: Apache-2.0 */
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#define V(name) PFN_##name name = (PFN_##name)gip(instance, #name); if (!name) return 2
#define OK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { printf("X11_RESIZE_ERROR %s result=%d\n", #call, r); return 2; } } while (0)

/* Keep an acquired image across a real X resize or native-window destruction.
 * Both suboptimal and rejected presents must consume their semaphore before
 * that same binary semaphore can be signaled again. Wait for the rejected present queue operations before re-signaling;
 * an empty submit with a fence then bounds the independent reuse check. */
int x11_surface_change(PFN_vkGetInstanceProcAddr gip, VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, VkQueue queue, xcb_connection_t *connection, xcb_window_t *window,
    VkSwapchainCreateInfoKHR *sw, VkSwapchainKHR *chain, VkCommandBuffer command,
    VkFence fence, const VkSemaphore *ready_by_image, unsigned epoch, unsigned width, unsigned height, int lost)
{
    V(vkAcquireNextImageKHR); V(vkWaitForFences); V(vkResetFences); V(vkGetFenceStatus);
    V(vkGetSwapchainImagesKHR); V(vkResetCommandBuffer); V(vkBeginCommandBuffer);
    V(vkCmdPipelineBarrier); V(vkEndCommandBuffer); V(vkQueueSubmit); V(vkQueuePresentKHR); V(vkQueueWaitIdle);
    V(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); V(vkCreateSwapchainKHR); V(vkDestroySwapchainKHR);
    uint32_t held, count = 8; VkImage images[8];
    OK(vkGetSwapchainImagesKHR(device, *chain, &count, images));
    OK(vkAcquireNextImageKHR(device, *chain, 2000000000ull, VK_NULL_HANDLE, fence, &held));
    if (held >= count) return 2;
    VkSemaphore ready = ready_by_image[held];
    OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
    OK(vkResetFences(device, 1, &fence));
    OK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    OK(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[held], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
    OK(vkEndCommandBuffer(command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
        .pCommandBuffers = &command, .signalSemaphoreCount = 1, .pSignalSemaphores = &ready};
    OK(vkQueueSubmit(queue, 1, &submit, fence));
    OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
    OK(vkResetFences(device, 1, &fence));
    VkResult expected = lost ? VK_ERROR_SURFACE_LOST_KHR : VK_SUBOPTIMAL_KHR;
    uint32_t dimensions[] = {width, height};
    xcb_generic_error_t *error = xcb_request_check(connection,
        lost ? xcb_destroy_window_checked(connection, *window) :
        xcb_configure_window_checked(connection, *window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions));
    if (error) { free(error); return 2; }
    if (lost) *window = XCB_NONE;
    VkSurfaceCapabilitiesKHR caps;
    VkResult queried = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, sw->surface, &caps);
    printf("X11_CHANGE_CAPS lost=%d result=%d\n", lost, queried);
    if (lost ? queried != expected : queried != VK_SUCCESS || caps.currentExtent.width != width || caps.currentExtent.height != height) return 2;
    uint32_t untouched = UINT32_MAX;
    VkResult acquired = vkAcquireNextImageKHR(device, *chain, 0, VK_NULL_HANDLE, fence, &untouched);
    printf("X11_RESIZE_ACQUIRE epoch=%u result=%d index=%u\n", epoch, acquired, untouched);
    if (!lost && acquired == VK_ERROR_OUT_OF_DATE_KHR) expected = acquired;
    if (acquired != expected) return 2;
    if (acquired == VK_SUBOPTIMAL_KHR) {
        if (untouched >= count || untouched == held) return 2;
        OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
        OK(vkResetFences(device, 1, &fence));
        /* This acquired image has no GPU use and may remain acquired until
         * swapchain destruction. The originally held image is presented. */
    } else if (untouched != UINT32_MAX || vkGetFenceStatus(device, fence) != VK_NOT_READY) return 2;
    VkResult per_chain = VK_SUCCESS;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &ready, .swapchainCount = 1,
        .pSwapchains = chain, .pImageIndices = &held, .pResults = &per_chain};
    VkResult presented = vkQueuePresentKHR(queue, &present);
    printf("X11_RESIZE_PRESENT epoch=%u result=%d per_chain=%d\n", epoch, presented, per_chain);
    if (presented != expected || per_chain != presented) return 2;
    /* OUT_OF_DATE and SURFACE_LOST still enqueue the present waits. Complete those queue
     * operations before this binary semaphore is signaled again. This probe
     * does not use swapchain-maintenance presentation fences. */
    OK(vkQueueWaitIdle(queue));
    submit.commandBufferCount = 0;
    OK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    submit.signalSemaphoreCount = 0; submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &ready; submit.pWaitDstStageMask = &stage;
    OK(vkQueueSubmit(queue, 1, &submit, fence));
    OK(vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
    OK(vkResetFences(device, 1, &fence));
    if (lost) {
        printf("X11_SURFACE_LOST capabilities=1 acquire=1 present=1 index_unchanged=1 fence_unsignaled=1 present_wait_idle=1 semaphore_reused=1\n");
        return 0;
    }
    sw->oldSwapchain = *chain; sw->imageExtent = caps.currentExtent;
    VkSwapchainKHR replacement;
    OK(vkCreateSwapchainKHR(device, sw, NULL, &replacement));
    uint32_t old_count = 8; VkImage old_images[8];
    OK(vkGetSwapchainImagesKHR(device, *chain, &old_count, old_images));
    if (old_count != count) return 2;
    for (unsigned i = 0; i < count; ++i) if (old_images[i] != images[i]) return 2;
    vkDestroySwapchainKHR(device, *chain, NULL);
    *chain = replacement; sw->oldSwapchain = VK_NULL_HANDLE;
    printf("X11_RESIZE epoch=%u size=%ux%u status=%s acquire_sync_checked=1 present_wait_idle=1 semaphore_reused=1 old_images_preserved=1\n", epoch, width, height,
        expected == VK_SUBOPTIMAL_KHR ? "SUBOPTIMAL" : "OUT_OF_DATE");
    return 0;
}
