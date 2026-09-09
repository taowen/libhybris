/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_MEMORY_VISIBILITY_H
#define HYBRIS_MEMORY_VISIBILITY_H
#include <vulkan/vulkan.h>
PFN_vkVoidFunction hybris_memory_visibility_proc(const char *name);
void hybris_memory_visibility_release_device(VkDevice device);
struct hybris_icd_device;
struct hybris_readback_range;
VkResult hybris_memory_visibility_readback_range(const struct hybris_icd_device *device,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, struct hybris_readback_range *range);
VkResult hybris_memory_visibility_invalidate(const struct hybris_icd_device *device,
    const struct hybris_readback_range *range);
#endif
