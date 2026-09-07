/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SWAPCHAIN_REVIEW_H
#define HYBRIS_SWAPCHAIN_REVIEW_H
#include <vulkan/vulkan.h>
int swapchain_review(PFN_vkGetInstanceProcAddr resolver, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
    const VkSwapchainCreateInfoKHR *model);
#endif
