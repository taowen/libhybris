/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_MEMORY_VISIBILITY_H
#define HYBRIS_MEMORY_VISIBILITY_H
#include <vulkan/vulkan.h>
PFN_vkVoidFunction hybris_memory_visibility_proc(const char *name);
void hybris_memory_visibility_release_device(VkDevice device);
struct hybris_layer_device;
struct hybris_readback_range;
/* Read-only buffer metadata for GPU vertex fetch. Preserve original usage;
 * unsupported/non-storage buffers select a whole filled draw instead of CPU
 * readback or implicit staging. Generation checks reject stale allocation
 * handles; caller still owns Vulkan lifetime/external synchronization. */
VkResult hybris_memory_visibility_storage_buffer(const struct hybris_layer_device *device,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkDescriptorBufferInfo *out);
VkResult hybris_memory_visibility_readback_range(const struct hybris_layer_device *device,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, struct hybris_readback_range *range);
VkResult hybris_memory_visibility_invalidate(const struct hybris_layer_device *device,
    const struct hybris_readback_range *range);
VkResult hybris_memory_visibility_buffer_size(const struct hybris_layer_device *device, VkBuffer buffer, VkDeviceSize *size);
#endif
