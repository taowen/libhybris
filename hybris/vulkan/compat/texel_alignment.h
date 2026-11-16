/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_TEXEL_ALIGNMENT_H
#define HYBRIS_TEXEL_ALIGNMENT_H
#include "../layer/layer.h"
VkDeviceSize hybris_texel_alignment(VkPhysicalDevice physical);
void hybris_texel_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *properties);
PFN_vkVoidFunction hybris_texel_proc(const char *name);
#endif
