/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SCALED_FORMATS_H
#define HYBRIS_SCALED_FORMATS_H
#include "shader_dispatch.h"
#define HYBRIS_SCALED_FORMAT_COUNT 13
struct hybris_scaled_format_pair { VkFormat scaled, integer; int is_signed, rb_swizzle; };
extern const struct hybris_scaled_format_pair hybris_scaled_formats[HYBRIS_SCALED_FORMAT_COUNT];
unsigned hybris_scaled_physical_mask(VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceFormatProperties query);
unsigned hybris_scaled_mask(VkDevice device, VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceFormatProperties query);
#endif
