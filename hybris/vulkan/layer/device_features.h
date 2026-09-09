/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <vulkan/vulkan.h>

struct hybris_device_features {
    VkDeviceCreateInfo info;
    VkPhysicalDeviceFeatures features;
    VkBaseOutStructure *copies;
    const void *suffix;
};

VkResult hybris_device_features_prepare(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator,
    struct hybris_device_features *out);
void hybris_device_features_release(struct hybris_device_features *filtered,
    const VkAllocationCallbacks *allocator);
