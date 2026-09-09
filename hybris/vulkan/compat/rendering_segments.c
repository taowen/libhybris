/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "rendering_segments.h"
#include "application_policy.h"
#include "scaled_vertex.h"
#include "../icd/commands.h"
#include <string.h>

static VkRenderingAttachmentInfo attachment(const VkRenderingAttachmentInfo *original,
    VkRenderingFlags flags)
{
    VkRenderingAttachmentInfo result = *original;
    if (!result.imageView) return result;
    /* Suspension ignores stores/resolves; resumption ignores loads. Ordinary
     * render segments must explicitly retain those intermediate contents. */
    if (flags & VK_RENDERING_RESUMING_BIT) result.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (flags & VK_RENDERING_SUSPENDING_BIT) {
        result.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        result.resolveMode = VK_RESOLVE_MODE_NONE;
        result.resolveImageView = VK_NULL_HANDLE;
        result.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return result;
}

static void begin_rendering(VkCommandBuffer command, const VkRenderingInfo *info, const char *name)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    PFN_vkCmdBeginRendering begin = (PFN_vkCmdBeginRendering)device.resolver(device.handle, name);
    if (!(device.application_policy & HYBRIS_APP_RENDERING_SEGMENTS)) {
        begin(command, info);
        return;
    }
    VkRenderingInfo rendering = *info;
    VkRenderingAttachmentInfo local_colors[8], depth, stencil;
    VkRenderingAttachmentInfo *colors = local_colors;
    VkAllocationCallbacks command_allocator;
    const VkAllocationCallbacks *allocator = hybris_icd_command_allocator(command, &command_allocator) ?
        &command_allocator : NULL;
    VkRenderingFlags segments = info->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
    if (segments) {
        if (info->colorAttachmentCount > sizeof(local_colors) / sizeof(*local_colors)) {
            uint64_t bytes = (uint64_t)info->colorAttachmentCount * sizeof(*colors);
            colors = bytes <= SIZE_MAX ? hybris_scaled_alloc(allocator, (size_t)bytes,
                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) : NULL;
            if (!colors) {
                /* Keep backend recording structurally intact, but make the
                 * allocation failure observable at vkEndCommandBuffer. */
                hybris_icd_command_error(command, VK_ERROR_OUT_OF_HOST_MEMORY);
                begin(command, info);
                return;
            }
        }
        for (uint32_t i = 0; i < info->colorAttachmentCount; ++i)
            colors[i] = attachment(&info->pColorAttachments[i], segments);
        rendering.pColorAttachments = info->colorAttachmentCount ? colors : NULL;
        if (info->pDepthAttachment) {
            depth = attachment(info->pDepthAttachment, segments);
            rendering.pDepthAttachment = &depth;
        }
        if (info->pStencilAttachment) {
            stencil = attachment(info->pStencilAttachment, segments);
            rendering.pStencilAttachment = &stencil;
        }
        rendering.flags &= ~(VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
    }
    /* The affected render graph also omits dependencies between its ordinary
     * segments. This is a command-buffer dependency, not a CPU queue-idle wait. */
    VkMemoryBarrier dependency = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)
        device.resolver(device.handle, "vkCmdPipelineBarrier");
    barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &dependency, 0, NULL, 0, NULL);
    begin(command, &rendering);
    if (colors != local_colors) hybris_scaled_free(allocator, colors);
}
static void VKAPI_CALL begin_core(VkCommandBuffer command, const VkRenderingInfo *info)
{ begin_rendering(command, info, "vkCmdBeginRendering"); }
static void VKAPI_CALL begin_khr(VkCommandBuffer command, const VkRenderingInfo *info)
{ begin_rendering(command, info, "vkCmdBeginRenderingKHR"); }

PFN_vkVoidFunction hybris_rendering_segments_proc(const char *name)
{
    if (!strcmp(name, "vkCmdBeginRendering")) return (PFN_vkVoidFunction)begin_core;
    if (!strcmp(name, "vkCmdBeginRenderingKHR")) return (PFN_vkVoidFunction)begin_khr;
    return NULL;
}
