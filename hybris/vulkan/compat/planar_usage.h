/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Adapted from MaliBridge common/vkb_caps.c and common/vkb_emu_formats.h.
 * Modified for libhybris physical-device dispatch.
 */
#ifndef HYBRIS_PLANAR_USAGE_H
#define HYBRIS_PLANAR_USAGE_H
#include "../layer/layer.h"

static inline int hybris_multiplanar(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM: case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16: case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16: case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16: case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM: case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM: case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM: case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16: case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
        return 1;
    default: return 0;
    }
}

/* Mali rejects EXTENDED_USAGE on planar formats even when every requested
 * usage is native. Remove only the redundant flag; views and usages that
 * need extended format support must retain the driver's original answer. */
static inline VkImageCreateFlags hybris_planar_usage(VkPhysicalDevice physical,
    VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    if (!(flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) || !hybris_multiplanar(format)) return flags;
    if (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR) return flags;
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return flags;
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceFormatProperties");
    if (!query) return flags;
    VkFormatProperties props = {0};
    query(physical, format, &props);
    VkFormatFeatureFlags have = tiling == VK_IMAGE_TILING_OPTIMAL ? props.optimalTilingFeatures : props.linearTilingFeatures;
    static const struct { VkImageUsageFlags usage; VkFormatFeatureFlags feature; } need[] = {
        {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
    };
    VkImageUsageFlags left = usage;
    for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); ++i) {
        if (!(usage & need[i].usage)) continue;
        if (!(have & need[i].feature)) return flags;
        left &= ~need[i].usage;
    }
    return left ? flags : flags & ~(VkImageCreateFlags)VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
}
#endif
