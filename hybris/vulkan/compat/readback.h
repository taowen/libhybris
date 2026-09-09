/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_READBACK_H
#define HYBRIS_READBACK_H
#include "../layer/layer.h"

/* Allocation identity and atom-aligned range captured while recording. No
 * pointer into an application mapping survives a command or submission. */
struct hybris_readback_range {
    VkDeviceMemory memory;
    uint64_t generation;
    VkDeviceSize offset, size;
};
struct hybris_readback_write {
    struct hybris_readback_range range;
    struct hybris_readback_write *next;
};
void hybris_readback_free_writes(struct hybris_readback_write *writes,
    const VkAllocationCallbacks *allocator);
VkResult hybris_readback_collect(VkCommandBuffer command,
    struct hybris_readback_write **writes, const VkAllocationCallbacks *allocator);
void hybris_readback_reset_command(VkCommandBuffer command);
void hybris_readback_reset_pool(VkDevice device, VkCommandPool pool);
void hybris_readback_release_submissions(VkDevice device);
PFN_vkVoidFunction hybris_readback_record_proc(const char *name);
PFN_vkVoidFunction hybris_readback_submit_proc(const char *name);
#endif
