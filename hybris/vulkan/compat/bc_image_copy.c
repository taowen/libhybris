/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)device->resolver(device->handle, "vk" #name)
/* Copy-compatible color formats: sizes/extents from the Khronos vk.xml format
 * registry. Two compressed formats must have identical block extents. */
static uint32_t block_geometry(VkFormat format, VkExtent3D *block)
{
    switch (format) {
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_USCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R64_UINT:
    case VK_FORMAT_R64_SINT:
    case VK_FORMAT_R64_SFLOAT:
    case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
    case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16:
        *block = (VkExtent3D){1,1,1}; return 8;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R64G64_UINT:
    case VK_FORMAT_R64G64_SINT:
    case VK_FORMAT_R64G64_SFLOAT:
        *block = (VkExtent3D){1,1,1}; return 16;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
    case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
    case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
    case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
    case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
        *block = (VkExtent3D){4,4,1}; return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
    case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK:
        *block = (VkExtent3D){4,4,1}; return 16;
    case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16:
    case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16:
    case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16:
    case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16:
    case VK_FORMAT_G16B16G16R16_422_UNORM:
    case VK_FORMAT_B16G16R16G16_422_UNORM:
        *block = (VkExtent3D){2,1,1}; return 8;
    default: return 0;
    }
}
static VkExtent3D mip_extent(const struct hybris_bc_resource *resource, uint32_t level)
{
    VkExtent3D extent = resource->extent;
    extent.width >>= level; extent.height >>= level; extent.depth >>= level;
    if (!extent.width) extent.width = 1;
    if (!extent.height) extent.height = 1;
    if (!extent.depth) extent.depth = 1;
    return extent;
}
static VkDeviceSize block_offset(const struct hybris_bc_image *image,
    const VkImageSubresourceLayers *subresource, VkOffset3D offset, uint32_t layer, uint32_t row)
{
    VkExtent3D extent = hybris_bc_image_extent(image, subresource->mipLevel);
    VkDeviceSize stride = ((extent.width + UINT64_C(3)) / 4) * image->block_bytes;
    return image->mip_offset[subresource->mipLevel] +
        ((VkDeviceSize)subresource->baseArrayLayer + layer) *
            hybris_bc_image_layer_size(image, subresource->mipLevel) +
        ((uint32_t)offset.y / 4 + row) * stride +
        (uint32_t)offset.x / 4 * (VkDeviceSize)image->block_bytes;
}
static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }
static void copy_images(struct hybris_bc_device *device, struct hybris_bc_command *command,
    const struct hybris_bc_resource *source, VkImageLayout source_layout,
    const struct hybris_bc_resource *destination, VkImageLayout destination_layout,
    uint32_t count, const VkImageCopy *regions)
{
    if (command->state.error != VK_SUCCESS) return;
    VkExtent3D source_block, destination_block;
    uint32_t bytes = block_geometry(source->format, &source_block);
    if (!bytes || bytes != block_geometry(destination->format, &destination_block)) {
        command->state.error = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return;
    }
    PROC(CmdCopyBuffer); PROC(CmdCopyImageToBuffer); PROC(CmdCopyBufferToImage);
    for (uint32_t i = 0; i < count; ++i) {
        const VkImageCopy *region = &regions[i];
        uint32_t columns = (region->extent.width + (uint64_t)source_block.width - 1) / source_block.width;
        uint32_t rows = (region->extent.height + (uint64_t)source_block.height - 1) / source_block.height;
        uint32_t layers = source->type == VK_IMAGE_TYPE_3D ? region->extent.depth :
            region->srcSubresource.layerCount;
        if (layers == VK_REMAINING_ARRAY_LAYERS) layers = source->layers - region->srcSubresource.baseArrayLayer;
        VkExtent3D destination_extent = region->extent;
        VkExtent3D bound = mip_extent(destination, region->dstSubresource.mipLevel);
        if (source_block.width != destination_block.width)
            destination_extent.width = minimum((uint64_t)columns * destination_block.width,
                bound.width - (uint32_t)region->dstOffset.x);
        if (source_block.height != destination_block.height)
            destination_extent.height = minimum((uint64_t)rows * destination_block.height,
                bound.height - (uint32_t)region->dstOffset.y);
        for (uint32_t layer = 0; layer < layers; ++layer) {
            for (uint32_t row = 0; row < rows; ++row) {
                if (source->emulated.image && destination->emulated.image) {
                    VkBufferCopy copy = {
                        .srcOffset = block_offset(&source->emulated, &region->srcSubresource, region->srcOffset, layer, row),
                        .dstOffset = block_offset(&destination->emulated, &region->dstSubresource, region->dstOffset, layer, row),
                        .size = (VkDeviceSize)columns * bytes};
                    CmdCopyBuffer(command->handle, source->emulated.blocks, destination->emulated.blocks, 1, &copy);
                    continue;
                }
                int to_bc = destination->emulated.image != VK_NULL_HANDLE;
                const struct hybris_bc_resource *native = to_bc ? source : destination;
                const struct hybris_bc_resource *bc = to_bc ? destination : source;
                VkExtent3D block = to_bc ? source_block : destination_block;
                VkExtent3D extent = to_bc ? region->extent : destination_extent;
                VkBufferImageCopy copy = {
                    .bufferOffset = block_offset(&bc->emulated,
                        to_bc ? &region->dstSubresource : &region->srcSubresource,
                        to_bc ? region->dstOffset : region->srcOffset, layer, row),
                    .imageSubresource = to_bc ? region->srcSubresource : region->dstSubresource,
                    .imageOffset = to_bc ? region->srcOffset : region->dstOffset,
                    .imageExtent = {extent.width, minimum(block.height, extent.height - row * block.height), 1}};
                copy.imageSubresource.layerCount = 1;
                if (native->type == VK_IMAGE_TYPE_3D) copy.imageOffset.z += layer;
                else copy.imageSubresource.baseArrayLayer += layer;
                copy.imageOffset.y += row * block.height;
                if (to_bc) CmdCopyImageToBuffer(command->handle, native->handle, source_layout,
                    bc->emulated.blocks, 1, &copy);
                else CmdCopyBufferToImage(command->handle, bc->emulated.blocks, native->handle,
                    destination_layout, 1, &copy);
            }
        }
        if (destination->emulated.image) {
            VkBufferImageCopy decode = {.imageSubresource = region->dstSubresource,
                .imageOffset = region->dstOffset, .imageExtent = destination_extent};
            decode.imageSubresource.layerCount = layers;
            decode.imageExtent.depth = 1;
            if (hybris_bc_decode_image(&command->transfer, command->handle, &command->state,
                &destination->emulated, destination_layout, &decode) != VK_SUCCESS) return;
        }
    }
    if (destination->emulated.image && count) hybris_bc_restore_compute(&command->state, command->handle);
}
static void VKAPI_CALL copy_image(VkCommandBuffer handle, VkImage source, VkImageLayout source_layout,
    VkImage destination, VkImageLayout destination_layout, uint32_t count, const VkImageCopy *regions)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    struct hybris_bc_resource *src = hybris_bc_resource_find(device, source);
    struct hybris_bc_resource *dst = hybris_bc_resource_find(device, destination);
    if (!(src && src->emulated.image) && !(dst && dst->emulated.image)) {
        PROC(CmdCopyImage);
        CmdCopyImage(handle, source, source_layout, destination, destination_layout, count, regions);
    } else if (src && dst) {
        copy_images(device, command, src, source_layout, dst, destination_layout, count, regions);
    } else {
        command->state.error = VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
}
static void copy_image2(VkCommandBuffer handle, const VkCopyImageInfo2 *info, const char *name)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    struct hybris_bc_resource *src = hybris_bc_resource_find(device, info->srcImage);
    struct hybris_bc_resource *dst = hybris_bc_resource_find(device, info->dstImage);
    if (!(src && src->emulated.image) && !(dst && dst->emulated.image)) {
        PFN_vkCmdCopyImage2 copy = (PFN_vkCmdCopyImage2)device->resolver(device->handle, name);
        copy(handle, info);
        return;
    }
    if (!src || !dst || info->pNext) { command->state.error = VK_ERROR_FEATURE_NOT_PRESENT; return; }
    if ((uint64_t)info->regionCount * sizeof(VkImageCopy) > SIZE_MAX) {
        command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY; return;
    }
    VkImageCopy *regions = hybris_bc_alloc(device, (size_t)info->regionCount * sizeof(*regions));
    if (!regions) { command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkImageCopy2 *region = &info->pRegions[i];
        if (region->pNext) {
            command->state.error = VK_ERROR_FEATURE_NOT_PRESENT;
            hybris_bc_free(device, regions);
            return;
        }
        regions[i] = (VkImageCopy){.srcSubresource = region->srcSubresource, .srcOffset = region->srcOffset,
            .dstSubresource = region->dstSubresource, .dstOffset = region->dstOffset, .extent = region->extent};
    }
    copy_images(device, command, src, info->srcImageLayout, dst, info->dstImageLayout, info->regionCount, regions);
    hybris_bc_free(device, regions);
}
static void VKAPI_CALL copy_image2_core(VkCommandBuffer command, const VkCopyImageInfo2 *info)
{ copy_image2(command, info, "vkCmdCopyImage2"); }
static void VKAPI_CALL copy_image2_khr(VkCommandBuffer command, const VkCopyImageInfo2 *info)
{ copy_image2(command, info, "vkCmdCopyImage2KHR"); }
PFN_vkVoidFunction hybris_bc_image_copy_proc(const char *name)
{
    if (!strcmp(name, "vkCmdCopyImage")) return (PFN_vkVoidFunction)copy_image;
    if (!strcmp(name, "vkCmdCopyImage2")) return (PFN_vkVoidFunction)copy_image2_core;
    if (!strcmp(name, "vkCmdCopyImage2KHR")) return (PFN_vkVoidFunction)copy_image2_khr;
    return NULL;
}
