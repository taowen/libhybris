/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "application_policy.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>

static int trace_policy(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_POLICY_TRACE");
    return value && !strcmp(value, "1");
}

unsigned hybris_application_policy(const VkApplicationInfo *app)
{
    if (trace_policy() && app)
        fprintf(stderr, "HYBRIS_POLICY_INSTANCE application=%s engine=%s applicationVersion=%u engineVersion=%u apiVersion=%u\n",
            app->pApplicationName ? app->pApplicationName : "", app->pEngineName ? app->pEngineName : "",
            app->applicationVersion, app->engineVersion, app->apiVersion);
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
    if (trace_policy()) {
        fprintf(stderr, "HYBRIS_POLICY_DEVICE candidate=0x%x extensions=%u\n", policy, info->enabledExtensionCount);
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            fprintf(stderr, "HYBRIS_POLICY_EXTENSION %s\n", info->ppEnabledExtensionNames[i]);
    }
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
        /* The observed stencil-export extension set has rendering evidence,
         * but no validated upload/readback profile. Select its bounded policy
         * here for all drivers instead of a separate Turnip-only layer path. */
        int known = !strcmp(info->ppEnabledExtensionNames[i], "VK_EXT_shader_stencil_export");
        if (known) policy &= HYBRIS_APP_RENDERING_SEGMENTS;
        for (size_t j = 0; j < sizeof(rendering_extensions) / sizeof(*rendering_extensions); ++j)
            if (!strcmp(info->ppEnabledExtensionNames[i], rendering_extensions[j])) { known = 1; break; }
        if (!known) {
            if (trace_policy()) fprintf(stderr, "HYBRIS_POLICY_DISABLED unsupported_extension=%s\n", info->ppEnabledExtensionNames[i]);
            return 0;
        }
    }
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
            ((const VkDeviceGroupDeviceCreateInfo *)node)->physicalDeviceCount != 1)
            policy &= ~(HYBRIS_APP_RENDERING_SEGMENTS | HYBRIS_APP_HOST_READBACK_INVALIDATE);
    return policy;
}
