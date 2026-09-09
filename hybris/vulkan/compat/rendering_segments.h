/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_RENDERING_SEGMENTS_H
#define HYBRIS_RENDERING_SEGMENTS_H
#include <vulkan/vulkan.h>
/* Shared by the HAL ICD and the standard loader layer. No backend state is
 * accessed here. On allocation failure no command is emitted; the adapter
 * must preserve recording structure and report the error at EndCommandBuffer. */
VkResult hybris_rendering_segments_begin(VkCommandBuffer command, const VkRenderingInfo *info,
    PFN_vkCmdBeginRendering begin, PFN_vkCmdPipelineBarrier barrier,
    const VkAllocationCallbacks *allocator);
PFN_vkVoidFunction hybris_rendering_segments_proc(const char *name);
#endif
