/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_DUAL_SOURCE_H
#define HYBRIS_DUAL_SOURCE_H
#include <vulkan/vulkan.h>
#include <stddef.h>
int hybris_dual_source_active(VkPhysicalDevice physical);
int hybris_dual_source_requested(const VkDeviceCreateInfo *info);
VkBlendFactor hybris_dual_source_factor(VkBlendFactor factor);
int hybris_dual_source_outputs(const uint32_t *code,size_t bytes);
VkResult hybris_dual_source_spirv(uint32_t *code,size_t bytes,uint32_t attachments,uint32_t limit);
#endif
