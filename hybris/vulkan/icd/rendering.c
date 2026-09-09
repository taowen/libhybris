/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "commands.h"
#include "../compat/application_policy.h"
#include "../compat/rendering_segments.h"
#include <string.h>

static void begin_rendering(VkCommandBuffer command, const VkRenderingInfo *info, const char *name)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    PFN_vkCmdBeginRendering begin = (PFN_vkCmdBeginRendering)device.resolver(device.handle, name);
    if (!(device.application_policy & HYBRIS_APP_RENDERING_SEGMENTS)) {
        begin(command, info);
        return;
    }
    VkAllocationCallbacks command_allocator;
    const VkAllocationCallbacks *allocator = hybris_icd_command_allocator(command, &command_allocator) ?
        &command_allocator : NULL;
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)
        device.resolver(device.handle, "vkCmdPipelineBarrier");
    VkResult result = hybris_rendering_segments_begin(command, info, begin, barrier, allocator);
    if (result != VK_SUCCESS) {
        hybris_icd_command_error(command, result);
        begin(command, info);
    }
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
