/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vulkan/vulkan.h>
struct hybris_layer_device;
struct hybris_dynamic_state;
struct hybris_dynamic_recording { VkBool32 active; };
int hybris_dynamic_emulated(VkPhysicalDevice physical);
void hybris_dynamic_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features);
VkResult hybris_dynamic_attach(struct hybris_layer_device *device,
    PFN_vkVoidFunction (*resolve)(const struct hybris_layer_device *, const char *),
    const VkDeviceCreateInfo *info);
void hybris_dynamic_release(struct hybris_dynamic_state *state);
PFN_vkVoidFunction hybris_dynamic_proc(const struct hybris_layer_device *device, const char *name);
PFN_vkVoidFunction hybris_dynamic_dispatch(const char *name);
int hybris_dynamic_native_extension(VkPhysicalDevice physical, const char *name);
