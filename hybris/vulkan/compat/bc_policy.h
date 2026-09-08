/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_POLICY_H
#define HYBRIS_BC_POLICY_H
#include <vulkan/vulkan.h>
int hybris_bc_enabled(void);
unsigned hybris_bc_physical_mask(VkPhysicalDevice physical);
int hybris_bc_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties);
int hybris_bc_extension_allowed(const char *name);
VkResult hybris_bc_prepare_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info);
VkResult hybris_bc_attach_device(VkDevice device, VkPhysicalDevice physical,
    PFN_vkGetDeviceProcAddr resolver, const VkAllocationCallbacks *allocator);
PFN_vkVoidFunction hybris_bc_policy_proc(const char *name);
PFN_vkVoidFunction hybris_bc_proc(const char *name);
#endif
