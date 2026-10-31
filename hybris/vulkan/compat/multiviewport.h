/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_MULTIVIEWPORT_H
#define HYBRIS_MULTIVIEWPORT_H
#include <vulkan/vulkan.h>
int hybris_multiviewport_active(VkPhysicalDevice physical);
int hybris_multiviewport_requested(const VkDeviceCreateInfo *info);
/* Discard all but index zero. Reports each discarded state category once. */
uint32_t hybris_multiviewport_count(unsigned category, uint32_t first, uint32_t count);
#endif
