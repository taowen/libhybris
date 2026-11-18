/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vulkan/vulkan.h>

struct hybris_layer_device;
struct hybris_timeline;

int hybris_timeline_enabled(void);
int hybris_timeline_emulated(VkPhysicalDevice physical);
int hybris_timeline_requested(const VkDeviceCreateInfo *info);
VkResult hybris_timeline_attach(struct hybris_layer_device *device);
void hybris_timeline_release(struct hybris_timeline *timeline);
int hybris_timeline_host_command(const char *name);
PFN_vkVoidFunction hybris_timeline_dispatch(const char *name);
PFN_vkVoidFunction hybris_timeline_submit_proc(const struct hybris_layer_device *device, const char *name);
void hybris_timeline_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out);
void hybris_timeline_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out);
