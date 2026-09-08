/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_transfer.h"
#include <stdlib.h>
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)t->decoder->resolver(t->decoder->device, "vk" #name)
struct hybris_bc_transfer_pool {
    VkDescriptorPool handle;
    uint32_t used;
    struct hybris_bc_transfer_pool *next;
};
void hybris_bc_transfer_init(struct hybris_bc_transfer *t,
    const struct hybris_bc_decoder *decoder, const VkPhysicalDeviceMemoryProperties *memory,
    const VkPhysicalDeviceLimits *limits)
{
    memset(t, 0, sizeof(*t));
    t->decoder = decoder;
    t->memory = memory;
    t->limits = limits;
}
void hybris_bc_transfer_finish(struct hybris_bc_transfer *t)
{
    PROC(DestroyDescriptorPool); PROC(DestroyBuffer); PROC(FreeMemory);
    while (t->pools) {
        struct hybris_bc_transfer_pool *next = t->pools->next;
        DestroyDescriptorPool(t->decoder->device, t->pools->handle, NULL);
        free(t->pools);
        t->pools = next;
    }
    if (t->scratch) DestroyBuffer(t->decoder->device, t->scratch, NULL);
    if (t->scratch_memory) FreeMemory(t->decoder->device, t->scratch_memory, NULL);
    hybris_bc_transfer_init(t, t->decoder, t->memory, t->limits);
}
static VkResult scratch(struct hybris_bc_transfer *t)
{
    if (t->scratch) return VK_SUCCESS;
    PROC(CreateBuffer); PROC(DestroyBuffer); PROC(GetBufferMemoryRequirements2);
    PROC(AllocateMemory); PROC(FreeMemory); PROC(BindBufferMemory);
    VkDevice device = t->decoder->device;
    VkDeviceSize size = t->limits->maxStorageBufferRange;
    if (size > 65536) size = 65536;
    if (size < 64) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = CreateBuffer(device, &info, NULL, &buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkBufferMemoryRequirementsInfo2 query = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buffer};
    GetBufferMemoryRequirements2(device, &query, &requirements);
    uint32_t type = t->memory->memoryTypeCount;
    for (uint32_t i = 0; i < t->memory->memoryTypeCount; ++i) {
        if (!(requirements.memoryRequirements.memoryTypeBits & (1u << i))) continue;
        if (type == t->memory->memoryTypeCount) type = i;
        if (t->memory->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; }
    }
    if (type == t->memory->memoryTypeCount) { result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto fail; }
    VkMemoryDedicatedAllocateInfo owner = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buffer};
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation ? &owner : NULL,
        .allocationSize = requirements.memoryRequirements.size, .memoryTypeIndex = type};
    result = AllocateMemory(device, &allocation, NULL, &memory);
    if (result != VK_SUCCESS) goto fail;
    result = BindBufferMemory(device, buffer, memory, 0);
    if (result != VK_SUCCESS) goto fail;
    t->scratch = buffer;
    t->scratch_memory = memory;
    t->scratch_size = size;
    return VK_SUCCESS;
fail:
    DestroyBuffer(device, buffer, NULL);
    if (memory) FreeMemory(device, memory, NULL);
    return result;
}
static VkResult descriptors(struct hybris_bc_transfer *t, VkDescriptorSet *set)
{
    PROC(CreateDescriptorPool); PROC(AllocateDescriptorSets);
    if (!t->pools || t->pools->used == 64) {
        struct hybris_bc_transfer_pool *pool = calloc(1, sizeof(*pool));
        if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 128};
        VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 64, .poolSizeCount = 1, .pPoolSizes = &size};
        VkResult result = CreateDescriptorPool(t->decoder->device, &info, NULL, &pool->handle);
        if (result != VK_SUCCESS) { free(pool); return result; }
        pool->next = t->pools;
        t->pools = pool;
    }
    VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = t->pools->handle, .descriptorSetCount = 1,
        .pSetLayouts = &t->decoder->descriptor_layout};
    VkResult result = AllocateDescriptorSets(t->decoder->device, &allocation, set);
    if (result == VK_SUCCESS) ++t->pools->used;
    return result;
}
static void buffer_barrier(struct hybris_bc_transfer *t, VkCommandBuffer command,
    VkBuffer buffer, VkPipelineStageFlags before, VkAccessFlags source,
    VkPipelineStageFlags after, VkAccessFlags destination)
{
    PROC(CmdPipelineBarrier);
    VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = source, .dstAccessMask = destination,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .size = VK_WHOLE_SIZE};
    CmdPipelineBarrier(command, before, after, 0, 0, NULL, 1, &barrier, 0, NULL);
}
static VkResult decode(struct hybris_bc_transfer *t, VkCommandBuffer command,
    const struct hybris_bc_image *image, VkImageLayout layout, const VkBufferImageCopy *region)
{
    VkResult result = scratch(t);
    if (result != VK_SUCCESS) return result;
    PROC(UpdateDescriptorSets); PROC(CmdCopyBufferToImage);
    uint32_t mip = region->imageSubresource.mipLevel;
    VkExtent3D extent = hybris_bc_image_extent(image, mip);
    VkDeviceSize row_bytes = ((extent.width + UINT64_C(3)) / 4) * image->block_bytes;
    VkDeviceSize alignment = t->limits->minStorageBufferOffsetAlignment;
    if (!alignment) alignment = 1;
    uint32_t texel_bytes = image->decoded_format == VK_FORMAT_R16G16B16_SFLOAT ? 6 :
        image->decoded_format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    uint32_t max_width = (uint32_t)(t->scratch_size / (16 * texel_bytes)) * 4;
    if (!max_width) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (uint32_t layer = 0; layer < region->imageSubresource.layerCount; ++layer) {
        for (uint32_t y = 0; y < region->imageExtent.height; y += 4) {
            uint32_t height = region->imageExtent.height - y;
            if (height > 4) height = 4;
            for (uint32_t x = 0; x < region->imageExtent.width;) {
                uint32_t width = region->imageExtent.width - x;
                if (width > max_width) width = max_width;
                VkDeviceSize source = image->mip_offset[mip] +
                    ((VkDeviceSize)region->imageSubresource.baseArrayLayer + layer) * hybris_bc_image_layer_size(image, mip) +
                    ((uint32_t)region->imageOffset.y / 4 + y / 4) * row_bytes +
                    ((uint32_t)region->imageOffset.x / 4 + x / 4) * (VkDeviceSize)image->block_bytes;
                VkDeviceSize base = source - source % alignment;
                VkDeviceSize available = t->limits->maxStorageBufferRange;
                if (source - base >= available) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                available = (available - (source - base)) / image->block_bytes;
                if (!available) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                if ((width + UINT64_C(3)) / 4 > available) width = (uint32_t)(available * 4);
                VkDeviceSize range = source - base + ((width + UINT64_C(3)) / 4) * image->block_bytes;
                VkDescriptorSet set;
                result = descriptors(t, &set);
                if (result != VK_SUCCESS) return result;
                VkDescriptorBufferInfo buffers[2] = {
                    {.buffer = image->blocks, .offset = base, .range = range},
                    {.buffer = t->scratch, .range = ((VkDeviceSize)width * height * texel_bytes + 3) & ~UINT64_C(3)}};
                VkWriteDescriptorSet writes[2] = {0};
                for (uint32_t i = 0; i < 2; ++i) {
                    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet = set;
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo = &buffers[i];
                }
                UpdateDescriptorSets(t->decoder->device, 2, writes, 0, NULL);
                /* Include the first use: the same recording can be submitted
                 * again, with the preceding execution still reading scratch. */
                buffer_barrier(t, command, t->scratch, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
                struct hybris_bc_region decoding = {.format = image->format,
                    .rgb16 = image->decoded_format == VK_FORMAT_R16G16B16_SFLOAT,
                    .rgb8 = image->decoded_format == VK_FORMAT_R8G8B8_UNORM || image->decoded_format == VK_FORMAT_R8G8B8_SRGB,
                    .width = width, .height = height, .layers = 1,
                    .source_offset = source - base, .source_range = range,
                    .destination_range = buffers[1].range};
                result = hybris_bc_decode_record(t->decoder, command, set, &decoding, t->limits);
                if (result != VK_SUCCESS) return result;
                buffer_barrier(t, command, t->scratch, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy copy = {.imageSubresource = region->imageSubresource,
                    .imageOffset = {region->imageOffset.x + (int32_t)x, region->imageOffset.y + (int32_t)y, 0},
                    .imageExtent = {width, height, 1}};
                copy.imageSubresource.baseArrayLayer += layer;
                copy.imageSubresource.layerCount = 1;
                CmdCopyBufferToImage(command, t->scratch, image->image, layout, 1, &copy);
                x += width;
            }
        }
    }
    return VK_SUCCESS;
}
VkResult hybris_bc_decode_image(struct hybris_bc_transfer *t,
    VkCommandBuffer command, struct hybris_bc_command_state *state,
    const struct hybris_bc_image *image, VkImageLayout layout, const VkBufferImageCopy *region)
{
    if (state->error != VK_SUCCESS) return state->error;
    buffer_barrier(t, command, image->blocks, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkResult result = decode(t, command, image, layout, region);
    if (result != VK_SUCCESS) { state->error = result; return result; }
    buffer_barrier(t, command, image->blocks, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    return VK_SUCCESS;
}
VkResult hybris_bc_copy_buffer_image(struct hybris_bc_transfer *t,
    VkCommandBuffer command, struct hybris_bc_command_state *state,
    const struct hybris_bc_image *image, VkImageLayout layout, VkBuffer buffer,
    uint32_t count, const VkBufferImageCopy *regions, int to_image)
{
    PROC(CmdCopyBuffer);
    if (state->error != VK_SUCCESS) return state->error;
    for (uint32_t i = 0; i < count; ++i) {
        VkBufferImageCopy normalized = regions[i];
        if (normalized.imageSubresource.layerCount == VK_REMAINING_ARRAY_LAYERS)
            normalized.imageSubresource.layerCount = image->layers - normalized.imageSubresource.baseArrayLayer;
        const VkBufferImageCopy *region = &normalized;
        uint32_t mip = region->imageSubresource.mipLevel;
        VkExtent3D extent = hybris_bc_image_extent(image, mip);
        VkDeviceSize row = ((extent.width + UINT64_C(3)) / 4) * image->block_bytes;
        VkDeviceSize buffer_row = (((region->bufferRowLength ? region->bufferRowLength :
            region->imageExtent.width) + UINT64_C(3)) / 4) * image->block_bytes;
        VkDeviceSize buffer_height = ((region->bufferImageHeight ? region->bufferImageHeight :
            region->imageExtent.height) + UINT64_C(3)) / 4;
        VkDeviceSize rows = (region->imageExtent.height + UINT64_C(3)) / 4;
        VkDeviceSize width = ((region->imageExtent.width + UINT64_C(3)) / 4) * image->block_bytes;
        for (uint32_t layer = 0; layer < region->imageSubresource.layerCount; ++layer) {
            VkDeviceSize image_offset = image->mip_offset[mip] +
                ((VkDeviceSize)region->imageSubresource.baseArrayLayer + layer) * hybris_bc_image_layer_size(image, mip) +
                ((uint32_t)region->imageOffset.y / 4) * row +
                ((uint32_t)region->imageOffset.x / 4) * (VkDeviceSize)image->block_bytes;
            VkDeviceSize buffer_offset = region->bufferOffset + layer * buffer_row * buffer_height;
            for (VkDeviceSize y = 0; y < rows; ++y) {
                VkBufferCopy copy = {.srcOffset = to_image ? buffer_offset + y * buffer_row : image_offset + y * row,
                    .dstOffset = to_image ? image_offset + y * row : buffer_offset + y * buffer_row, .size = width};
                CmdCopyBuffer(command, to_image ? buffer : image->blocks, to_image ? image->blocks : buffer, 1, &copy);
            }
        }
        if (to_image) {
            VkResult result = hybris_bc_decode_image(t, command, state, image, layout, region);
            if (result != VK_SUCCESS) return result;
        }
    }
    if (to_image && count) hybris_bc_restore_compute(state, command);
    return VK_SUCCESS;
}
