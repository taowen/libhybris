/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_TRANSFER_H
#define HYBRIS_BC_TRANSFER_H
#include "bc_decode.h"
#include "bc_image.h"
#include "bc_command_state.h"
struct hybris_bc_transfer_pool;
struct hybris_bc_transfer {
    const struct hybris_bc_decoder *decoder;
    const VkPhysicalDeviceMemoryProperties *memory;
    const VkPhysicalDeviceLimits *limits;
    struct hybris_bc_transfer_pool *pools;
    VkBuffer scratch;
    VkDeviceMemory scratch_memory;
    VkDeviceSize scratch_size;
};
void hybris_bc_transfer_init(struct hybris_bc_transfer *transfer,
    const struct hybris_bc_decoder *decoder, const VkPhysicalDeviceMemoryProperties *memory,
    const VkPhysicalDeviceLimits *limits);
/* Retire only when the owning command buffer may be reset/freed. */
void hybris_bc_transfer_finish(struct hybris_bc_transfer *transfer);
VkResult hybris_bc_decode_image(struct hybris_bc_transfer *transfer,
    VkCommandBuffer command, struct hybris_bc_command_state *state,
    const struct hybris_bc_image *image, VkImageLayout layout, const VkBufferImageCopy *region);
VkResult hybris_bc_copy_buffer_image(struct hybris_bc_transfer *transfer,
    VkCommandBuffer command, struct hybris_bc_command_state *state,
    const struct hybris_bc_image *image, VkImageLayout layout, VkBuffer buffer,
    uint32_t count, const VkBufferImageCopy *regions, int to_image);
#endif
