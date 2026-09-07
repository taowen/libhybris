/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_DECODE_H
#define HYBRIS_BC_DECODE_H
#include <vulkan/vulkan.h>

/* Internal GPU kernel, not an advertisement of Vulkan compressed-image support.
 * Source/destination are non-overlapping storage-buffer descriptor ranges.
 * Offsets below are relative to those ranges, not to bound device memory.
 * The caller owns buffers/descriptors and their synchronization/lifetime.
 * Recording changes compute pipeline, set 0 and compute push constants: a
 * command interceptor MUST preserve application state before using this API.
 * No queue submission, host mapping or implicit wait is performed here. */
struct hybris_bc_decoder {
    VkDevice device;
    PFN_vkGetDeviceProcAddr resolver;
    VkDescriptorSetLayout descriptor_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
};
struct hybris_bc_region {
    VkFormat format;
    uint32_t width, height, layers;
    uint32_t row_length, image_height; /* zero means tightly packed */
    VkDeviceSize source_offset, destination_offset;
    VkDeviceSize source_range, destination_range;
};
struct hybris_bc_push {
    uint32_t mode, source_word, row_blocks, layer_blocks;
    uint32_t width, height, pixels, first_pixel, destination_word, chunk_pixels;
};
VkResult hybris_bc_decoder_create(VkDevice device, PFN_vkGetDeviceProcAddr resolver,
    const VkAllocationCallbacks *allocator, struct hybris_bc_decoder *decoder);
void hybris_bc_decoder_destroy(struct hybris_bc_decoder *decoder,
    const VkAllocationCallbacks *allocator);
/* Checks all arithmetic, range and dispatch bounds before recording anything.
 * BC4-7 return FORMAT_NOT_SUPPORTED. Invalid internal regions return
 * INITIALIZATION_FAILED. This API does not validate application Vulkan usage. */
VkResult hybris_bc_prepare(const struct hybris_bc_region *region,
    const VkPhysicalDeviceLimits *limits, struct hybris_bc_push *push);
VkResult hybris_bc_decode_record(const struct hybris_bc_decoder *decoder,
    VkCommandBuffer command, VkDescriptorSet descriptors,
    const struct hybris_bc_region *region, const VkPhysicalDeviceLimits *limits);
#endif
