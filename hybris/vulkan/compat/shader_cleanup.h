/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SHADER_CLEANUP_H
#define HYBRIS_SHADER_CLEANUP_H
#include <vulkan/vulkan.h>
int hybris_shader_cleanup_enabled(void);
VkResult VKAPI_CALL hybris_shader_cleanup_create(VkDevice device,
    const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *allocator, VkShaderModule *module);
PFN_vkVoidFunction hybris_shader_cleanup_proc(const char *name);
#endif
