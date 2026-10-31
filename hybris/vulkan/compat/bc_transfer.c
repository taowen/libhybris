/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded scratch allocation and batched copies adapted from MaliBridge
 * 95a1e600a51299294e06bb09db2db8cc509fe6a3, server/server_emu_bcn.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors.
 */
#define VK_NO_PROTOTYPES
#include "bc_transfer.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)t->decoder->resolver(t->decoder->device, "vk" #name)
static pthread_once_t quality_once = PTHREAD_ONCE_INIT;
static unsigned quality = 3;
static void configure_quality(void)
{
    if (getauxval(AT_SECURE)) return;
    const char *value = getenv("HYBRIS_BC_TRANSCODE_QUALITY");
    if (!value || !strcmp(value, "auto")) return;
    if (!strcmp(value, "fast")) quality = 0;
    else if (!strcmp(value, "default")) quality = 1;
    else if (!strcmp(value, "high")) quality = 2;
    else fprintf(stderr, "HYBRIS_BC_TRANSCODE_QUALITY: expected auto, fast, default or high; using auto\n");
}
static unsigned level_quality(VkExtent3D extent)
{
    pthread_once(&quality_once, configure_quality);
    return quality != 3 ? quality : (uint64_t)extent.width * extent.height >= UINT64_C(1024) * 1024 ? 0 : 1;
}
struct hybris_bc_transfer_pool {
    VkDescriptorPool handle;
    uint32_t used;
    struct hybris_bc_transfer_pool *next;
};
/* MaliBridge's budgets apply to buffer payloads. The live limit is soft:
 * a command with no chunk may still acquire the minimum needed to progress. */
#define CHUNK_MIN (UINT64_C(1) << 20)
#define CHUNK_MAX (UINT64_C(16) << 20)
#define CB_MAX CHUNK_MAX
#define IDLE_MAX (UINT64_C(32) << 20)
#define LIVE_MAX (UINT64_C(96) << 20)
struct hybris_bc_scratch {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size, used;
    struct hybris_bc_scratch *next;
};
static pthread_mutex_t scratch_guard = PTHREAD_MUTEX_INITIALIZER;
static VkDeviceSize scratch_live, scratch_idle;

void hybris_bc_transfer_init(struct hybris_bc_transfer *t,
    struct hybris_bc_decoder *decoder, const VkPhysicalDeviceMemoryProperties *memory,
    const VkPhysicalDeviceLimits *limits)
{
    memset(t, 0, sizeof(*t));
    t->decoder = decoder;
    t->memory = memory;
    t->limits = limits;
}
static void chunk_destroy(struct hybris_bc_decoder *d, struct hybris_bc_scratch *c)
{
    ((PFN_vkDestroyBuffer)d->resolver(d->device, "vkDestroyBuffer"))(d->device, c->buffer, NULL);
    ((PFN_vkFreeMemory)d->resolver(d->device, "vkFreeMemory"))(d->device, c->memory, NULL);
    pthread_mutex_lock(&scratch_guard);
    scratch_live -= c->size;
    pthread_mutex_unlock(&scratch_guard);
    free(c);
}
void hybris_bc_transfer_device_finish(struct hybris_bc_decoder *d)
{
    /* Device destruction is externally synchronized; all held chunks retired. */
    while (d->idle_scratch) {
        struct hybris_bc_scratch *c = d->idle_scratch;
        d->idle_scratch = c->next;
        pthread_mutex_lock(&scratch_guard);
        scratch_idle -= c->size;
        pthread_mutex_unlock(&scratch_guard);
        chunk_destroy(d, c);
    }
}
void hybris_bc_transfer_finish(struct hybris_bc_transfer *t)
{
    PROC(DestroyDescriptorPool);
    while (t->pools) {
        struct hybris_bc_transfer_pool *next = t->pools->next;
        DestroyDescriptorPool(t->decoder->device, t->pools->handle, NULL);
        free(t->pools);
        t->pools = next;
    }
    while (t->chunks) {
        struct hybris_bc_scratch *c = t->chunks;
        t->chunks = c->next;
        pthread_mutex_lock(&scratch_guard);
        int keep = scratch_idle + c->size <= IDLE_MAX;
        if (keep) {
            scratch_idle += c->size;
            c->used = 0;
            c->next = t->decoder->idle_scratch;
            t->decoder->idle_scratch = c;
        }
        pthread_mutex_unlock(&scratch_guard);
        if (!keep) chunk_destroy(t->decoder, c);
    }
    hybris_bc_transfer_init(t, t->decoder, t->memory, t->limits);
}
static VkDeviceSize chunk_size(VkDeviceSize want, VkDeviceSize limit)
{
    VkDeviceSize size = CHUNK_MIN < limit ? CHUNK_MIN : limit;
    while (size < want && size <= limit / 2) size *= 2;
    return size;
}
static VkResult chunk_get(struct hybris_bc_transfer *t, VkDeviceSize need,
    VkDeviceSize want, struct hybris_bc_scratch **out)
{
    *out = NULL;
    VkDeviceSize limit = t->limits->maxStorageBufferRange;
    if (limit > CHUNK_MAX) limit = CHUNK_MAX;
    if (need > limit || limit < 64) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (want > limit) want = limit;
    pthread_mutex_lock(&scratch_guard);
    struct hybris_bc_scratch **best = NULL;
    for (struct hybris_bc_scratch **p = &t->decoder->idle_scratch; *p; p = &(*p)->next)
        if ((*p)->size >= need && (!best || (*p)->size < (*best)->size)) best = p;
    if (best) {
        *out = *best;
        *best = (*out)->next;
        scratch_idle -= (*out)->size;
        pthread_mutex_unlock(&scratch_guard);
        (*out)->next = NULL;
        return VK_SUCCESS;
    }
    VkDeviceSize size = chunk_size(want, limit);
    if (scratch_live + size > LIVE_MAX) {
        if (t->chunks) { pthread_mutex_unlock(&scratch_guard); return VK_SUCCESS; }
        size = chunk_size(need, limit);
    }
    scratch_live += size;
    pthread_mutex_unlock(&scratch_guard);
    struct hybris_bc_scratch *c = calloc(1, sizeof(*c));
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!c) goto fail;
    c->size = size;
    PROC(CreateBuffer); PROC(GetBufferMemoryRequirements2); PROC(AllocateMemory); PROC(BindBufferMemory);
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    result = CreateBuffer(t->decoder->device, &info, NULL, &c->buffer);
    if (result != VK_SUCCESS) goto fail;
    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkBufferMemoryRequirementsInfo2 query = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = c->buffer};
    GetBufferMemoryRequirements2(t->decoder->device, &query, &req);
    uint32_t type = t->memory->memoryTypeCount;
    for (uint32_t i = 0; i < t->memory->memoryTypeCount; ++i) {
        if (!(req.memoryRequirements.memoryTypeBits & (1u << i))) continue;
        if (type == t->memory->memoryTypeCount) type = i;
        if (t->memory->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; }
    }
    if (type == t->memory->memoryTypeCount) { result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto fail; }
    VkMemoryDedicatedAllocateInfo owner = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = c->buffer};
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation ? &owner : NULL,
        .allocationSize = req.memoryRequirements.size, .memoryTypeIndex = type};
    result = AllocateMemory(t->decoder->device, &allocation, NULL, &c->memory);
    if (result != VK_SUCCESS) goto fail;
    result = BindBufferMemory(t->decoder->device, c->buffer, c->memory, 0);
    if (result != VK_SUCCESS) goto fail;
    *out = c;
    return VK_SUCCESS;
fail:
    if (c) {
        if (c->buffer) ((PFN_vkDestroyBuffer)t->decoder->resolver(t->decoder->device, "vkDestroyBuffer"))(t->decoder->device, c->buffer, NULL);
        if (c->memory) ((PFN_vkFreeMemory)t->decoder->resolver(t->decoder->device, "vkFreeMemory"))(t->decoder->device, c->memory, NULL);
        free(c);
    }
    pthread_mutex_lock(&scratch_guard); scratch_live -= size; pthread_mutex_unlock(&scratch_guard);
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
struct upload {
    struct hybris_bc_transfer *transfer;
    VkCommandBuffer command;
    const struct hybris_bc_image *image;
    VkImageLayout layout;
    VkBuffer buffers[64];
    VkBufferImageCopy copies[64];
    uint32_t count;
};
static void to_compute(struct upload *u)
{
    struct hybris_bc_transfer *t = u->transfer;
    PROC(CmdPipelineBarrier);
    /* Also cover scratch read by the previous execution of this recording. */
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    CmdPipelineBarrier(u->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &b, 0, NULL, 0, NULL);
}
static void flush(struct upload *u)
{
    if (!u->count) return;
    struct hybris_bc_transfer *t = u->transfer;
    PROC(CmdPipelineBarrier); PROC(CmdCopyBufferToImage);
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    CmdPipelineBarrier(u->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &b, 0, NULL, 0, NULL);
    for (uint32_t i = 0; i < u->count;) {
        uint32_t end = i + 1;
        while (end < u->count && u->buffers[end] == u->buffers[i]) ++end;
        CmdCopyBufferToImage(u->command, u->buffers[i], u->image->image, u->layout, end - i, &u->copies[i]);
        i = end;
    }
    u->count = 0;
}
static VkResult slot(struct upload *u, VkDeviceSize want, VkDeviceSize alignment,
    struct hybris_bc_scratch **out, VkDeviceSize *offset)
{
    struct hybris_bc_transfer *t = u->transfer;
    for (;;) {
        if (t->current) {
            VkDeviceSize start = (t->current->used + alignment - 1) / alignment * alignment;
            if (start < t->current->size) { *out = t->current; *offset = start; return VK_SUCCESS; }
            t->current = t->current->next;
            continue;
        }
        if (t->held_bytes < CB_MAX) {
            struct hybris_bc_scratch *c;
            VkDeviceSize remaining = CB_MAX - t->held_bytes;
            VkResult result = chunk_get(t, 64, want < remaining ? want : remaining, &c);
            if (result != VK_SUCCESS) return result;
            if (c) {
                c->next = t->chunks;
                t->chunks = t->current = c;
                t->held_bytes += c->size;
                continue;
            }
        }
        /* Never overwrite output until its queued copies have consumed it. */
        flush(u);
        to_compute(u);
        for (struct hybris_bc_scratch *c = t->chunks; c; c = c->next) c->used = 0;
        t->current = t->chunks;
    }
}
static VkResult decode(struct hybris_bc_transfer *t, VkCommandBuffer command,
    const struct hybris_bc_image *image, VkImageLayout layout, const VkBufferImageCopy *region)
{
    VkResult result;
    struct upload upload = {.transfer=t, .command=command, .image=image, .layout=layout};
    to_compute(&upload);
    PROC(UpdateDescriptorSets);
    uint32_t mip = region->imageSubresource.mipLevel;
    VkExtent3D extent = hybris_bc_image_extent(image, mip);
    VkDeviceSize row_bytes = ((extent.width + UINT64_C(3)) / 4) * image->block_bytes;
    VkDeviceSize alignment = t->limits->minStorageBufferOffsetAlignment;
    if (!alignment) alignment = 1;
    struct hybris_bc_region representation = {.format = image->format, .output_format = image->decoded_format,
        .rgb16 = image->decoded_format == VK_FORMAT_R16G16B16_SFLOAT,
        .rgb8 = image->decoded_format == VK_FORMAT_R8G8B8_UNORM || image->decoded_format == VK_FORMAT_R8G8B8_SRGB};
    uint32_t block_bytes = hybris_bc_output_block_bytes(&representation);
    if (!block_bytes) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkDeviceSize slot_alignment = alignment > 256 ? alignment : 256;
    int volume = image->type == VK_IMAGE_TYPE_3D;
    uint32_t slices = volume ? region->imageExtent.depth : region->imageSubresource.layerCount;
    uint32_t first_slice = volume ? (uint32_t)region->imageOffset.z : region->imageSubresource.baseArrayLayer;
    for (uint32_t layer = 0; layer < slices; ++layer) {
        VkDeviceSize wanted = ((region->imageExtent.width + UINT64_C(3)) / 4) *
            ((region->imageExtent.height + UINT64_C(3)) / 4) * block_bytes;
        for (uint32_t y = 0; y < region->imageExtent.height;) {
            uint32_t height = region->imageExtent.height - y;
            /* Every x piece in a row band must have the same height. */
            uint32_t band_height = 0;
            for (uint32_t x = 0; x < region->imageExtent.width;) {
                struct hybris_bc_scratch *chunk;
                VkDeviceSize destination;
                result = slot(&upload, wanted, slot_alignment, &chunk, &destination);
                if (result != VK_SUCCESS) return result;
                VkDeviceSize available_blocks = (chunk->size - destination) / block_bytes;
                if (!available_blocks) { chunk->used = chunk->size; continue; }
                VkDeviceSize row_blocks = (region->imageExtent.width - x + UINT64_C(3)) / 4;
                if (!band_height && available_blocks < row_blocks && chunk->used) {
                    chunk->used = chunk->size;
                    continue;
                }
                uint32_t width = region->imageExtent.width - x;
                uint64_t max_width = available_blocks * 4;
                if (width > max_width) width = (uint32_t)max_width;
                uint32_t max_height = (uint32_t)(available_blocks / ((width + UINT64_C(3)) / 4) * 4);
                height = band_height ? band_height : region->imageExtent.height - y;
                if (height > max_height) height = max_height;
                /* Narrow x pieces arise only when the row cannot fit a slot. */
                if (width < region->imageExtent.width - x && !band_height && height > 4) height = 4;
                if (!band_height) band_height = height;
                if (height != band_height) { chunk->used = chunk->size; continue; }
                VkDeviceSize source = image->mip_offset[mip] +
                    ((VkDeviceSize)first_slice + layer) * hybris_bc_image_slice_size(image, mip) +
                    ((uint32_t)region->imageOffset.y / 4 + y / 4) * row_bytes +
                    ((uint32_t)region->imageOffset.x / 4 + x / 4) * (VkDeviceSize)image->block_bytes;
                VkDeviceSize base = source - source % alignment;
                VkDeviceSize available = t->limits->maxStorageBufferRange;
                if (source - base >= available) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                available = (available - (source - base)) / image->block_bytes;
                if (!available) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                if ((width + UINT64_C(3)) / 4 > available) width = (uint32_t)(available * 4);
                VkDeviceSize range = source - base + (((height + UINT64_C(3)) / 4 - 1) * row_bytes) +
                    ((width + UINT64_C(3)) / 4) * image->block_bytes;
                if (range > t->limits->maxStorageBufferRange) return VK_ERROR_FORMAT_NOT_SUPPORTED;
                VkDescriptorSet set;
                result = descriptors(t, &set);
                if (result != VK_SUCCESS) return result;
                VkDescriptorBufferInfo buffers[2] = {
                    {.buffer = image->blocks, .offset = base, .range = range},
                    {.buffer = chunk->buffer, .offset = destination,
                        .range = ((VkDeviceSize)width + 3) / 4 * ((height + UINT64_C(3)) / 4) * block_bytes}};
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
                struct hybris_bc_region decoding = {.format = image->format, .output_format = image->decoded_format,
                    .quality = level_quality(extent),
                    .rgb16 = image->decoded_format == VK_FORMAT_R16G16B16_SFLOAT,
                    .rgb8 = image->decoded_format == VK_FORMAT_R8G8B8_UNORM || image->decoded_format == VK_FORMAT_R8G8B8_SRGB,
                    .width = width, .height = height, .layers = 1,
                    .row_length = (extent.width + 3u) & ~3u,
                    .source_offset = source - base, .source_range = range,
                    .destination_range = buffers[1].range};
                result = hybris_bc_decode_record(t->decoder, command, set, &decoding, t->limits);
                if (result != VK_SUCCESS) return result;
                chunk->used = destination + buffers[1].range;
                VkBufferImageCopy copy = {.bufferOffset = destination, .bufferRowLength = (width + 3u) & ~3u,
                    .bufferImageHeight = (height + 3u) & ~3u, .imageSubresource = region->imageSubresource,
                    .imageOffset = {region->imageOffset.x + (int32_t)x, region->imageOffset.y + (int32_t)y, 0},
                    .imageExtent = {width, height, 1}};
                if (volume) copy.imageOffset.z = region->imageOffset.z + (int32_t)layer;
                else copy.imageSubresource.baseArrayLayer += layer;
                copy.imageSubresource.layerCount = 1;
                upload.buffers[upload.count] = chunk->buffer;
                upload.copies[upload.count++] = copy;
                if (upload.count == 64) { flush(&upload); to_compute(&upload); }
                wanted = wanted > buffers[1].range ? wanted - buffers[1].range : 0;
                x += width;
            }
            y += height;
        }
    }
    flush(&upload);
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
        int volume = image->type == VK_IMAGE_TYPE_3D;
        uint32_t slices = volume ? region->imageExtent.depth : region->imageSubresource.layerCount;
        uint32_t first_slice = volume ? (uint32_t)region->imageOffset.z : region->imageSubresource.baseArrayLayer;
        for (uint32_t layer = 0; layer < slices; ++layer) {
            VkDeviceSize image_offset = image->mip_offset[mip] +
                ((VkDeviceSize)first_slice + layer) * hybris_bc_image_slice_size(image, mip) +
                ((uint32_t)region->imageOffset.y / 4) * row +
                ((uint32_t)region->imageOffset.x / 4) * (VkDeviceSize)image->block_bytes;
            VkDeviceSize buffer_offset = region->bufferOffset + layer * buffer_row * buffer_height;
            VkBuffer source = to_image ? buffer : image->blocks;
            VkBuffer destination = to_image ? image->blocks : buffer;
            if (row == width && buffer_row == width) {
                VkBufferCopy copy = {.srcOffset = to_image ? buffer_offset : image_offset,
                    .dstOffset = to_image ? image_offset : buffer_offset, .size = width * rows};
                CmdCopyBuffer(command, source, destination, 1, &copy);
            } else {
                VkBufferCopy copies[64];
                const uint32_t capacity = sizeof(copies) / sizeof(copies[0]);
                for (VkDeviceSize y = 0; y < rows;) {
                    uint32_t n = (uint32_t)(rows - y > capacity ? capacity : rows - y);
                    for (uint32_t j = 0; j < n; ++j) copies[j] = (VkBufferCopy){
                        .srcOffset = to_image ? buffer_offset + (y + j) * buffer_row : image_offset + (y + j) * row,
                        .dstOffset = to_image ? image_offset + (y + j) * row : buffer_offset + (y + j) * buffer_row,
                        .size = width};
                    CmdCopyBuffer(command, source, destination, n, copies);
                    y += n;
                }
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
