/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_VULKAN_LAYER_H
#define HYBRIS_VULKAN_LAYER_H
#include <vulkan/vulkan.h>

struct hybris_layer_device {
    VkDevice handle;
    PFN_vkGetDeviceProcAddr resolver;
    uint64_t generation;
    unsigned policy;
};

/* Dispatchable child handles share their device's loader dispatch key.
 * Snapshots are valid for a Vulkan externally synchronized object lifetime. */
int hybris_layer_device(const void *handle, struct hybris_layer_device *out);
PFN_vkVoidFunction hybris_layer_commands_proc(const char *name);
void hybris_layer_commands_release(const struct hybris_layer_device *device);
#endif
