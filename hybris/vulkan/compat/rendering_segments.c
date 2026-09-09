/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "rendering_segments.h"
#include "scaled_vertex.h"

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

VkResult hybris_rendering_segments_begin(VkCommandBuffer command, const VkRenderingInfo *info,
    PFN_vkCmdBeginRendering begin, PFN_vkCmdPipelineBarrier barrier,
    const VkAllocationCallbacks *allocator)
{
    VkRenderingInfo rendering = *info;
    VkRenderingAttachmentInfo local_colors[8], depth, stencil;
    VkRenderingAttachmentInfo *colors = local_colors;
    VkRenderingFlags segments = info->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
    if (segments) {
        if (info->colorAttachmentCount > sizeof(local_colors) / sizeof(*local_colors)) {
            uint64_t bytes = (uint64_t)info->colorAttachmentCount * sizeof(*colors);
            colors = bytes <= SIZE_MAX ? hybris_scaled_alloc(allocator, (size_t)bytes,
                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) : NULL;
            if (!colors) return VK_ERROR_OUT_OF_HOST_MEMORY;
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
    barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &dependency, 0, NULL, 0, NULL);
    begin(command, &rendering);
    if (colors != local_colors) hybris_scaled_free(allocator, colors);
    return VK_SUCCESS;
}
