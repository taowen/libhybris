/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_BC_DECODE_H
#define HYBRIS_BC_DECODE_H
#include <vulkan/vulkan.h>
#include <pthread.h>
#define HYBRIS_BC_KERNEL_COUNT 17

/* Internal GPU kernel, not an advertisement of Vulkan compressed-image support.
 * Output is RGBA8 for BC1-3/BC7, R8 UNORM/SNORM for BC4, RG8 for BC5.
 * BC1 RGB can instead write RGB8. Scratch rows/layers are padded to 4x4 blocks;
 * output size is ceil(width/4)*ceil(height/4)*layers*output_block_bytes.
 * output_format may select packed BC1 or native EAC/ASTC 4x4 transcoding.
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
    VkPipeline pipelines[3][HYBRIS_BC_KERNEL_COUNT];
    pthread_mutex_t guard;
    VkAllocationCallbacks allocator;
    int custom_allocator;
};
struct hybris_bc_region {
    VkFormat format;
    VkFormat output_format; /* UNDEFINED selects the decoded representation. */
    uint32_t quality; /* 0 fast, 1 default, 2 high; chosen for the whole mip. */
    VkBool32 rgb16; /* BC6H only: packed RGB16F instead of RGBA16F output */
    VkBool32 rgb8; /* BC1 RGB only: packed RGB8 instead of RGBA8 output */
    uint32_t width, height, layers;
    uint32_t row_length, image_height; /* zero means tightly packed */
    VkDeviceSize source_offset, destination_offset;
    VkDeviceSize source_range, destination_range;
};
struct hybris_bc_push {
    uint32_t source_word, row_blocks, layer_blocks, flags;
    uint32_t destination_word, pad, width, height;
};
VkResult hybris_bc_decoder_create(VkDevice device, PFN_vkGetDeviceProcAddr resolver,
    const VkAllocationCallbacks *allocator, struct hybris_bc_decoder *decoder);
void hybris_bc_decoder_destroy(struct hybris_bc_decoder *decoder,
    const VkAllocationCallbacks *allocator);
/* Checks all arithmetic, range and dispatch bounds before recording anything.
 * Formats outside BC1–BC7 return FORMAT_NOT_SUPPORTED. BC6H outputs RGBA16F
 * bit patterns. Invalid internal regions return
 * INITIALIZATION_FAILED. This API does not validate application Vulkan usage. */
VkResult hybris_bc_prepare(const struct hybris_bc_region *region,
    const VkPhysicalDeviceLimits *limits, struct hybris_bc_push *push);
uint32_t hybris_bc_output_block_bytes(const struct hybris_bc_region *region);
VkResult hybris_bc_decode_record(struct hybris_bc_decoder *decoder,
    VkCommandBuffer command, VkDescriptorSet descriptors,
    const struct hybris_bc_region *region, const VkPhysicalDeviceLimits *limits);
#endif
