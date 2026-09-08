/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)device->resolver(device->handle, "vk" #name)
static uint32_t level_count(const struct hybris_bc_image *image, const VkImageSubresourceRange *range)
{
    return range->levelCount == VK_REMAINING_MIP_LEVELS ? image->mip_levels - range->baseMipLevel : range->levelCount;
}
static void backing_range(const struct hybris_bc_image *image, const VkImageSubresourceRange *range,
    uint32_t level, VkDeviceSize *offset, VkDeviceSize *size)
{
    uint32_t layers = range->layerCount == VK_REMAINING_ARRAY_LAYERS ? image->layers - range->baseArrayLayer : range->layerCount;
    VkDeviceSize layer_size = hybris_bc_image_layer_size(image, level);
    *offset = image->mip_offset[level] + layer_size * range->baseArrayLayer;
    *size = layer_size * layers;
}
static VkBufferMemoryBarrier *prepare(struct hybris_bc_device *device,
    struct hybris_bc_command *command, uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
    uint32_t image_count, const VkImageMemoryBarrier *images, uint32_t *out_count)
{
    *out_count = buffer_count;
    uint64_t count = buffer_count;
    for (uint32_t i = 0; i < image_count; ++i) {
        struct hybris_bc_resource *resource = hybris_bc_resource_find(device, images[i].image);
        if (resource && resource->emulated.image) count += level_count(&resource->emulated, &images[i].subresourceRange);
    }
    if (count == buffer_count) return NULL;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(VkBufferMemoryBarrier)) {
        command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    VkBufferMemoryBarrier *translated = hybris_bc_alloc(device, (size_t)count * sizeof(*translated));
    if (!translated) { command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY; return NULL; }
    if (buffer_count) memcpy(translated, buffers, (size_t)buffer_count * sizeof(*buffers));
    uint32_t index = buffer_count;
    for (uint32_t i = 0; i < image_count; ++i) {
        struct hybris_bc_resource *resource = hybris_bc_resource_find(device, images[i].image);
        if (!resource || !resource->emulated.image) continue;
        const struct hybris_bc_image *image = &resource->emulated;
        uint32_t levels = level_count(image, &images[i].subresourceRange);
        for (uint32_t j = 0; j < levels; ++j) {
            VkBufferMemoryBarrier *barrier = &translated[index++];
            *barrier = (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = images[i].srcAccessMask, .dstAccessMask = images[i].dstAccessMask,
                .srcQueueFamilyIndex = images[i].srcQueueFamilyIndex, .dstQueueFamilyIndex = images[i].dstQueueFamilyIndex,
                .buffer = image->blocks};
            backing_range(image, &images[i].subresourceRange, images[i].subresourceRange.baseMipLevel + j,
                &barrier->offset, &barrier->size);
        }
    }
    *out_count = index;
    return translated;
}
static VkBufferMemoryBarrier2 *prepare2(struct hybris_bc_device *device,
    struct hybris_bc_command *command, const VkDependencyInfo *info, VkDependencyInfo *out)
{
    *out = *info;
    uint64_t count = info->bufferMemoryBarrierCount;
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2 *barrier = &info->pImageMemoryBarriers[i];
        struct hybris_bc_resource *resource = hybris_bc_resource_find(device, barrier->image);
        if (resource && resource->emulated.image) count += level_count(&resource->emulated, &barrier->subresourceRange);
    }
    if (count == info->bufferMemoryBarrierCount) return NULL;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(VkBufferMemoryBarrier2)) {
        command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    VkBufferMemoryBarrier2 *translated = hybris_bc_alloc(device, (size_t)count * sizeof(*translated));
    if (!translated) { command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY; return NULL; }
    if (info->bufferMemoryBarrierCount)
        memcpy(translated, info->pBufferMemoryBarriers, (size_t)info->bufferMemoryBarrierCount * sizeof(*translated));
    uint32_t index = info->bufferMemoryBarrierCount;
    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2 *original = &info->pImageMemoryBarriers[i];
        struct hybris_bc_resource *resource = hybris_bc_resource_find(device, original->image);
        if (!resource || !resource->emulated.image) continue;
        const struct hybris_bc_image *image = &resource->emulated;
        uint32_t levels = level_count(image, &original->subresourceRange);
        for (uint32_t j = 0; j < levels; ++j) {
            VkBufferMemoryBarrier2 *barrier = &translated[index++];
            *barrier = (VkBufferMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = original->srcStageMask, .srcAccessMask = original->srcAccessMask,
                .dstStageMask = original->dstStageMask, .dstAccessMask = original->dstAccessMask,
                .srcQueueFamilyIndex = original->srcQueueFamilyIndex, .dstQueueFamilyIndex = original->dstQueueFamilyIndex,
                .buffer = image->blocks};
            backing_range(image, &original->subresourceRange, original->subresourceRange.baseMipLevel + j,
                &barrier->offset, &barrier->size);
        }
    }
    out->bufferMemoryBarrierCount = index;
    out->pBufferMemoryBarriers = translated;
    return translated;
}
static void VKAPI_CALL pipeline_barrier(VkCommandBuffer handle,
    VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags flags,
    uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
    const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    uint32_t count;
    VkBufferMemoryBarrier *translated = prepare(device, command, buffer_count, buffers, image_count, images, &count);
    if (command->state.error == VK_SUCCESS) {
        PROC(CmdPipelineBarrier);
        CmdPipelineBarrier(handle, source, destination, flags, memory_count, memory,
            count, translated ? translated : buffers, image_count, images);
    }
    hybris_bc_free(device, translated);
}
static void VKAPI_CALL wait_events(VkCommandBuffer handle, uint32_t event_count, const VkEvent *events,
    VkPipelineStageFlags source, VkPipelineStageFlags destination, uint32_t memory_count,
    const VkMemoryBarrier *memory, uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
    uint32_t image_count, const VkImageMemoryBarrier *images)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    uint32_t count;
    VkBufferMemoryBarrier *translated = prepare(device, command, buffer_count, buffers, image_count, images, &count);
    if (command->state.error == VK_SUCCESS) {
        PROC(CmdWaitEvents);
        CmdWaitEvents(handle, event_count, events, source, destination, memory_count, memory,
            count, translated ? translated : buffers, image_count, images);
    }
    hybris_bc_free(device, translated);
}
static void barrier_or_set2(VkCommandBuffer handle, VkEvent event, const VkDependencyInfo *info,
    const char *name, int set_event)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    VkDependencyInfo translated_info;
    VkBufferMemoryBarrier2 *translated = prepare2(device, command, info, &translated_info);
    if (command->state.error == VK_SUCCESS) {
        if (set_event) {
            PFN_vkCmdSetEvent2 record = (PFN_vkCmdSetEvent2)device->resolver(device->handle, name);
            record(handle, event, &translated_info);
        } else {
            PFN_vkCmdPipelineBarrier2 record = (PFN_vkCmdPipelineBarrier2)device->resolver(device->handle, name);
            record(handle, &translated_info);
        }
    }
    hybris_bc_free(device, translated);
}
static void wait_events2(VkCommandBuffer handle, uint32_t count, const VkEvent *events,
    const VkDependencyInfo *infos, const char *name)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    if ((uint64_t)count * sizeof(VkDependencyInfo) > SIZE_MAX ||
        (uint64_t)count * sizeof(VkBufferMemoryBarrier2 *) > SIZE_MAX) {
        command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    VkDependencyInfo *translated = count ? hybris_bc_alloc(device, (size_t)count * sizeof(*translated)) : NULL;
    VkBufferMemoryBarrier2 **owned = count ? hybris_bc_alloc(device, (size_t)count * sizeof(*owned)) : NULL;
    if (count && (!translated || !owned)) {
        command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY;
    } else {
        for (uint32_t i = 0; i < count && command->state.error == VK_SUCCESS; ++i)
            owned[i] = prepare2(device, command, &infos[i], &translated[i]);
        if (command->state.error == VK_SUCCESS) {
            PFN_vkCmdWaitEvents2 record = (PFN_vkCmdWaitEvents2)device->resolver(device->handle, name);
            record(handle, count, events, translated);
        }
    }
    if (owned) for (uint32_t i = 0; i < count; ++i) hybris_bc_free(device, owned[i]);
    hybris_bc_free(device, owned);
    hybris_bc_free(device, translated);
}
#define SYNC2(suffix) \
static void VKAPI_CALL barrier2_##suffix(VkCommandBuffer command, const VkDependencyInfo *info) \
{ barrier_or_set2(command, VK_NULL_HANDLE, info, "vkCmdPipelineBarrier2" #suffix, 0); } \
static void VKAPI_CALL set2_##suffix(VkCommandBuffer command, VkEvent event, const VkDependencyInfo *info) \
{ barrier_or_set2(command, event, info, "vkCmdSetEvent2" #suffix, 1); } \
static void VKAPI_CALL wait2_##suffix(VkCommandBuffer command, uint32_t count, const VkEvent *events, const VkDependencyInfo *infos) \
{ wait_events2(command, count, events, infos, "vkCmdWaitEvents2" #suffix); }
SYNC2()
SYNC2(KHR)
#undef SYNC2
PFN_vkVoidFunction hybris_bc_barriers_proc(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction function; } commands[] = {
#define ENTRY(name, function) {"vk" #name, (PFN_vkVoidFunction)function}
        ENTRY(CmdPipelineBarrier, pipeline_barrier), ENTRY(CmdWaitEvents, wait_events),
        ENTRY(CmdPipelineBarrier2, barrier2_), ENTRY(CmdPipelineBarrier2KHR, barrier2_KHR),
        ENTRY(CmdSetEvent2, set2_), ENTRY(CmdSetEvent2KHR, set2_KHR),
        ENTRY(CmdWaitEvents2, wait2_), ENTRY(CmdWaitEvents2KHR, wait2_KHR)
#undef ENTRY
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(name, commands[i].name)) return commands[i].function;
    return NULL;
}
