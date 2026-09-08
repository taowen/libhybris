/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)device->resolver(device->handle, "vk" #name)
static void copy_buffer_image(VkCommandBuffer handle, VkBuffer buffer, VkImage image,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions, int to_image)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    struct hybris_bc_resource *resource = hybris_bc_resource_find(device, image);
    if (resource && resource->emulated.image) {
        hybris_bc_copy_buffer_image(&command->transfer, handle, &command->state,
            &resource->emulated, layout, buffer, count, regions, to_image);
    } else if (to_image) {
        PROC(CmdCopyBufferToImage);
        CmdCopyBufferToImage(handle, buffer, image, layout, count, regions);
    } else {
        PROC(CmdCopyImageToBuffer);
        CmdCopyImageToBuffer(handle, image, layout, buffer, count, regions);
    }
}
static void VKAPI_CALL copy_to_image(VkCommandBuffer command, VkBuffer buffer, VkImage image,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions)
{ copy_buffer_image(command, buffer, image, layout, count, regions, 1); }
static void VKAPI_CALL copy_to_buffer(VkCommandBuffer command, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions)
{ copy_buffer_image(command, buffer, image, layout, count, regions, 0); }
static void copy_buffer_image2(VkCommandBuffer handle, VkBuffer buffer, VkImage image,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy2 *regions, int to_image,
    const void *info, const char *name)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    struct hybris_bc_resource *resource = hybris_bc_resource_find(device, image);
    if (!resource || !resource->emulated.image) {
        if (to_image) {
            PFN_vkCmdCopyBufferToImage2 copy = (PFN_vkCmdCopyBufferToImage2)device->resolver(device->handle, name);
            copy(handle, info);
        } else {
            PFN_vkCmdCopyImageToBuffer2 copy = (PFN_vkCmdCopyImageToBuffer2)device->resolver(device->handle, name);
            copy(handle, info);
        }
        return;
    }
    if (((const VkBaseInStructure *)info)->pNext) {
        command->state.error = VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    if ((uint64_t)count * sizeof(VkBufferImageCopy) > SIZE_MAX) {
        command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return;
    }
    VkBufferImageCopy *copies = count ? hybris_bc_alloc(device, (size_t)count * sizeof(*copies)) : NULL;
    if (count && !copies) { command->state.error = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
    for (uint32_t i = 0; i < count; ++i) {
        if (regions[i].pNext) {
            command->state.error = VK_ERROR_FEATURE_NOT_PRESENT;
            hybris_bc_free(device, copies);
            return;
        }
        copies[i] = (VkBufferImageCopy){.bufferOffset = regions[i].bufferOffset,
            .bufferRowLength = regions[i].bufferRowLength, .bufferImageHeight = regions[i].bufferImageHeight,
            .imageSubresource = regions[i].imageSubresource, .imageOffset = regions[i].imageOffset,
            .imageExtent = regions[i].imageExtent};
    }
    hybris_bc_copy_buffer_image(&command->transfer, handle, &command->state,
        &resource->emulated, layout, buffer, count, copies, to_image);
    hybris_bc_free(device, copies);
}
#define COPY2_TO_IMAGE(suffix) \
static void VKAPI_CALL copy_to_image2_##suffix(VkCommandBuffer command, const VkCopyBufferToImageInfo2 *info) \
{ copy_buffer_image2(command, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount, \
    info->pRegions, 1, info, "vkCmdCopyBufferToImage2" #suffix); }
#define COPY2_TO_BUFFER(suffix) \
static void VKAPI_CALL copy_to_buffer2_##suffix(VkCommandBuffer command, const VkCopyImageToBufferInfo2 *info) \
{ copy_buffer_image2(command, info->dstBuffer, info->srcImage, info->srcImageLayout, info->regionCount, \
    info->pRegions, 0, info, "vkCmdCopyImageToBuffer2" #suffix); }
COPY2_TO_IMAGE()
COPY2_TO_IMAGE(KHR)
COPY2_TO_BUFFER()
COPY2_TO_BUFFER(KHR)
#undef COPY2_TO_IMAGE
#undef COPY2_TO_BUFFER

PFN_vkVoidFunction hybris_bc_record_proc(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction function; } commands[] = {
#define ENTRY(name, function) {"vk" #name, (PFN_vkVoidFunction)function}
        ENTRY(CmdCopyBufferToImage, copy_to_image), ENTRY(CmdCopyImageToBuffer, copy_to_buffer),
        ENTRY(CmdCopyBufferToImage2, copy_to_image2_), ENTRY(CmdCopyBufferToImage2KHR, copy_to_image2_KHR),
        ENTRY(CmdCopyImageToBuffer2, copy_to_buffer2_), ENTRY(CmdCopyImageToBuffer2KHR, copy_to_buffer2_KHR)
#undef ENTRY
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(name, commands[i].name)) return commands[i].function;
    return NULL;
}
