/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vulkan/vulkan.h>
struct hybris_layer_device;
int hybris_maintenance5_emulated(VkPhysicalDevice physical);
int hybris_maintenance5_requested(const VkDeviceCreateInfo *info);
void hybris_maintenance5_features(VkPhysicalDevice physical,VkPhysicalDeviceFeatures2 *features);
void hybris_maintenance5_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *properties);
PFN_vkVoidFunction hybris_maintenance5_dispatch(const char *name);
PFN_vkVoidFunction hybris_maintenance5_proc(const struct hybris_layer_device *device,const char *name);
VkResult hybris_maintenance5_buffer_usage(const struct hybris_layer_device *device,
    const void *chain,VkBufferUsageFlags *usage);

PFN_vkVoidFunction hybris_maintenance5_added_proc(const char *name);
