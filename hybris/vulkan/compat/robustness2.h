/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vulkan/vulkan.h>
enum { HYBRIS_ROBUST_BUFFER=1, HYBRIS_ROBUST_IMAGE=2, HYBRIS_ROBUST_NULL=4 };
struct hybris_robustness2_policy {
    VkPhysicalDeviceRobustness2FeaturesEXT native;
    VkBool32 core_buffer, core_image, image_extension;
    unsigned native_extensions, emulated;
};
VkResult hybris_robustness2_policy(VkPhysicalDevice physical,struct hybris_robustness2_policy *out);
void hybris_robustness2_features(VkPhysicalDevice physical,VkPhysicalDeviceFeatures2 *features);
PFN_vkVoidFunction hybris_robustness2_physical_proc(const char *name);
