/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_IMAGE_H
#define HYBRIS_BC_IMAGE_H
#include <vulkan/vulkan.h>

struct hybris_bc_image {
    VkDevice device;
    PFN_vkGetDeviceProcAddr resolver;
    VkImage image;
    VkBuffer blocks;
    VkDeviceMemory decoded_memory;
    VkMemoryRequirements requirements;
    VkBool32 requires_dedicated, prefers_dedicated;
    VkFormat format, decoded_format;
    VkExtent3D extent;
    uint32_t mip_levels, layers, block_bytes;
    VkDeviceSize mip_offset[32], block_size;
};
VkFormat hybris_bc_image_format(VkFormat format, unsigned rgb_mask);
VkResult hybris_bc_image_describe(const VkImageCreateInfo *info, struct hybris_bc_image *image);
void hybris_bc_image_backing_info(const struct hybris_bc_image *image,
    const VkImageCreateInfo *info, VkBufferCreateInfo *backing);
VkResult hybris_bc_image_create(VkDevice device, PFN_vkGetDeviceProcAddr resolver,
    const VkPhysicalDeviceMemoryProperties *memory, const VkImageCreateInfo *info,
    unsigned rgb_mask, const VkAllocationCallbacks *allocator, struct hybris_bc_image *image);
void hybris_bc_image_destroy(struct hybris_bc_image *image,
    const VkAllocationCallbacks *allocator);
VkResult hybris_bc_image_bind(const struct hybris_bc_image *image,
    VkDeviceMemory memory, VkDeviceSize offset);
VkExtent3D hybris_bc_image_extent(const struct hybris_bc_image *image, uint32_t mip);
VkDeviceSize hybris_bc_image_layer_size(const struct hybris_bc_image *image, uint32_t mip);
#endif
