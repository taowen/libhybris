/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_VULKAN_LAYER_H
#define HYBRIS_VULKAN_LAYER_H
#include <vulkan/vulkan.h>

/* Vulkan state shared by every compatibility feature and driver. The layer
 * preserves driver handles and uses the loader dispatch key for child owners. */
struct hybris_layer_physical {
    VkInstance instance;
    uint64_t generation;
    uint32_t api_version;
    PFN_vkGetInstanceProcAddr resolver;
    unsigned application_policy;
};
struct hybris_layer_device {
    VkDevice handle;
    PFN_vkGetDeviceProcAddr resolver;
    VkPhysicalDevice physical;
    uint64_t generation;
    unsigned application_policy;
    int track_commands;
    uint64_t instance_generation;
};
int hybris_layer_lookup_physical(VkPhysicalDevice physical, struct hybris_layer_physical *out);
int hybris_layer_lookup_device(VkDevice device, struct hybris_layer_device *out);
int hybris_layer_lookup_queue(VkQueue queue, struct hybris_layer_device *out);
int hybris_layer_device_allocator(VkDevice device, VkAllocationCallbacks *allocator);
void *hybris_layer_dispatch_key(const void *handle);
void hybris_layer_trace_lifetime(const char *action, uint64_t generation,
    const void *handle, uint64_t instance_generation);
VkResult VKAPI_CALL hybris_layer_create_device(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *out);
void VKAPI_CALL hybris_layer_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator);
PFN_vkVoidFunction VKAPI_CALL hybris_layer_get_device_proc(VkDevice device, const char *name);
PFN_vkVoidFunction hybris_layer_device_inner_proc(VkDevice device, const char *name);
PFN_vkVoidFunction hybris_layer_device_dispatch(const char *name, int track_commands);
PFN_vkVoidFunction hybris_layer_physical_proc(const char *name);

int hybris_layer_command_device(VkCommandBuffer command, struct hybris_layer_device *device);
int hybris_layer_command_allocator(VkCommandBuffer command, VkAllocationCallbacks *allocator);
VkCommandPool hybris_layer_command_pool(VkCommandBuffer command);
void hybris_layer_command_error(VkCommandBuffer command, VkResult error);
void hybris_layer_commands_release_device(VkDevice device);
PFN_vkVoidFunction hybris_layer_commands_proc(const char *name);
#endif
