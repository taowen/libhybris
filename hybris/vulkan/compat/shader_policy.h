/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SHADER_POLICY_H
#define HYBRIS_SHADER_POLICY_H
#include <vulkan/vulkan.h>
unsigned hybris_shader_physical_mask(VkPhysicalDevice physical);
int hybris_shader_extension_allowed(const char *name);
int hybris_shader_command_allowed(const char *name);
VkResult hybris_shader_prepare_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info);
PFN_vkVoidFunction hybris_shader_policy_proc(const char *name);
#endif
