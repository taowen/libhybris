/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_MULTIDRAW_H
#define HYBRIS_MULTIDRAW_H
#include <vulkan/vulkan.h>
int hybris_multidraw_active(VkPhysicalDevice physical);
int hybris_multidraw_requested(const VkDeviceCreateInfo *info);
PFN_vkVoidFunction hybris_multidraw_proc(const char *name);
#endif
