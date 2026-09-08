/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SHADER_DISPATCH_H
#define HYBRIS_SHADER_DISPATCH_H
#include <vulkan/vulkan.h>
int hybris_scaled_enabled(void);
int hybris_shader_enabled(void);
void hybris_scaled_format(PFN_vkGetPhysicalDeviceFormatProperties query,
    VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties);
VkResult hybris_shader_device_create(VkDevice device, VkPhysicalDevice physical,
    PFN_vkGetDeviceProcAddr resolver, PFN_vkGetPhysicalDeviceFormatProperties query,
    const VkAllocationCallbacks *allocator);
void hybris_shader_device_destroy(VkDevice device);
PFN_vkVoidFunction hybris_shader_proc(const char *name);
#endif
