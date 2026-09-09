/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "application_policy.h"
#include <string.h>

unsigned hybris_application_policy(const VkApplicationInfo *app)
{
    /* Blender 4.3.2 reports 1.0.0 for both versions, not its release number.
     * Match the observed public API signature, never the executable path. */
    if (!app || !app->pApplicationName || !app->pEngineName ||
        strcmp(app->pApplicationName, "Blender") || strcmp(app->pEngineName, "Blender") ||
        app->applicationVersion != VK_MAKE_VERSION(1, 0, 0) ||
        app->engineVersion != VK_MAKE_VERSION(1, 0, 0) || app->apiVersion != VK_API_VERSION_1_2)
        return 0;
    return HYBRIS_APP_HOST_UPLOAD_FLUSH | HYBRIS_APP_RENDERING_SEGMENTS |
        HYBRIS_APP_HOST_READBACK_INVALIDATE;
}

unsigned hybris_application_device_policy(unsigned policy, const VkDeviceCreateInfo *info)
{
    /* Bound these workarounds to the observed backend's extension set.
     * In particular, implicit multisample resolves and tile-memory rendering
     * need separate treatment before they can use this lowering. */
    static const char *const rendering_extensions[] = {
        "VK_KHR_swapchain", "VK_KHR_dynamic_rendering",
        "VK_EXT_dynamic_rendering_unused_attachments", "VK_KHR_maintenance4",
        "VK_KHR_fragment_shader_barycentric", "VK_EXT_debug_marker",
        /* Replay enables external synchronization without changing rendering
         * semantics. Explicit depth/stencil resolve is already core in the
         * matched API 1.2 application and uses the attachment resolve fields. */
        "VK_KHR_external_fence_fd", "VK_KHR_external_semaphore_fd",
        "VK_KHR_depth_stencil_resolve",
    };
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        int known = 0;
        for (size_t j = 0; j < sizeof(rendering_extensions) / sizeof(*rendering_extensions); ++j)
            if (!strcmp(info->ppEnabledExtensionNames[i], rendering_extensions[j])) { known = 1; break; }
        if (!known) return 0;
    }
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
            ((const VkDeviceGroupDeviceCreateInfo *)node)->physicalDeviceCount != 1)
            policy &= ~(HYBRIS_APP_RENDERING_SEGMENTS | HYBRIS_APP_HOST_READBACK_INVALIDATE);
    return policy;
}
