/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "device_features.h"
#include "../compat/vertex_stores.h"
#include "../compat/clip_distance.h"
#include "../compat/bc_policy.h"
#include "../compat/scaled_vertex.h"
#include "../compat/shader_dispatch.h"
#include <vulkan/vk_layer.h>
#include <stdio.h>
#include <string.h>

static size_t node_size(VkStructureType type)
{
    switch (type) {
    case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: return sizeof(VkLayerDeviceCreateInfo);
#include "device_create_sizes.inc"
    default: return 0;
    }
}

void hybris_device_features_release(struct hybris_device_features *filtered,
    const VkAllocationCallbacks *allocator)
{
    hybris_scaled_free(allocator, filtered->extensions);
    filtered->extensions = NULL;
    VkBaseOutStructure *node = filtered->copies;
    while (node && node != filtered->suffix) {
        VkBaseOutStructure *next = node->pNext;
        hybris_scaled_free(allocator, node);
        node = next;
    }
    filtered->copies = NULL;
}

VkResult hybris_device_features_prepare(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator,
    struct hybris_device_features *out)
{
    *out = (struct hybris_device_features){.info = *info};
    int clip = hybris_clip_active(physical);
    int stores = hybris_vertex_stores_active(physical);
    int bc = hybris_bc_complete(physical);
    VkPhysicalDeviceFeatures compression = {0};
    hybris_bc_enable_transcode_features(physical, &compression);
    int transcode = compression.textureCompressionETC2 || compression.textureCompressionASTC_LDR;
    int hdr = hybris_bc_astc_hdr(physical);
    int cull = hybris_cull_active(physical);
    int polygon=hybris_shader_polygon_emulated(physical);
    if (!clip && !stores && !bc && !cull && !polygon && !transcode && !hdr) return VK_SUCCESS;
    if (info->pEnabledFeatures) {
        out->features = *info->pEnabledFeatures;
        if (transcode) hybris_bc_enable_transcode_features(physical, &out->features);
        if (clip) out->features.shaderClipDistance = VK_FALSE;
        if (stores) out->features.vertexPipelineStoresAndAtomics = VK_FALSE;
        if (bc) out->features.textureCompressionBC = VK_FALSE;
        if (cull) {
            out->features.shaderCullDistance = VK_FALSE;
            out->features.geometryShader = VK_TRUE;
        }
        if (polygon && out->features.fillModeNonSolid) {
            out->features.fillModeNonSolid=VK_FALSE;
            out->features.drawIndirectFirstInstance=VK_TRUE;
        }
        out->info.pEnabledFeatures = &out->features;
    }
    const VkBaseInStructure *features2 = NULL, *hdr_node = NULL, *target = NULL;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            features2 = node;
            target = node;
        }
        if (hdr && (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES ||
                    node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
            hdr_node = node;
            target = node;
        }
    }
    if (!features2 && !info->pEnabledFeatures && (cull || transcode)) {
        out->features = (VkPhysicalDeviceFeatures){.geometryShader = cull ? VK_TRUE : VK_FALSE};
        if (transcode) hybris_bc_enable_transcode_features(physical, &out->features);
        out->info.pEnabledFeatures = &out->features;
    }
    if (hdr == 2) {
        int present = 0;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            present |= !strcmp(info->ppEnabledExtensionNames[i], VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);
        if (!present) {
            out->extensions = hybris_scaled_alloc(allocator,
                (info->enabledExtensionCount + 1) * sizeof(*out->extensions), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if (!out->extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
            if (info->enabledExtensionCount)
                memcpy(out->extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*out->extensions));
            out->extensions[info->enabledExtensionCount] = VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME;
            out->info.ppEnabledExtensionNames = out->extensions;
            out->info.enabledExtensionCount++;
        }
    }
    if (!target) {
        if (hdr && !hdr_node) {
            out->hdr = (VkPhysicalDeviceTextureCompressionASTCHDRFeatures){
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES,
                .pNext = (void *)out->info.pNext, .textureCompressionASTC_HDR = VK_TRUE};
            out->info.pNext = &out->hdr;
        }
        return VK_SUCCESS;
    }

    /* Only copy the prefix needed to update core and ASTC HDR features. Preserve every other
     * payload and the untouched suffix; never write through application input
     * memory, which can be read-only or shared by concurrent device creates. */
    out->suffix = target->pNext;
    VkBaseOutStructure **slot = &out->copies;
    for (const VkBaseInStructure *node = info->pNext; ; node = node->pNext) {
        size_t size = node_size(node->sType);
        if (!size) {
            fprintf(stderr, "HYBRIS_DEVICE_FEATURES unknown prefix sType=%u\n", node->sType);
            hybris_device_features_release(out, allocator);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        VkBaseOutStructure *copy = hybris_scaled_alloc(allocator, size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!copy) {
            hybris_device_features_release(out, allocator);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        memcpy(copy, node, size);
        copy->pNext = NULL;
        *slot = copy;
        slot = &copy->pNext;
        if (node == features2) {
            if (transcode) hybris_bc_enable_transcode_features(physical, &((VkPhysicalDeviceFeatures2 *)copy)->features);
            if (clip) ((VkPhysicalDeviceFeatures2 *)copy)->features.shaderClipDistance = VK_FALSE;
            if (stores) ((VkPhysicalDeviceFeatures2 *)copy)->features.vertexPipelineStoresAndAtomics = VK_FALSE;
            if (bc) ((VkPhysicalDeviceFeatures2 *)copy)->features.textureCompressionBC = VK_FALSE;
            if (cull) {
                ((VkPhysicalDeviceFeatures2 *)copy)->features.shaderCullDistance = VK_FALSE;
                ((VkPhysicalDeviceFeatures2 *)copy)->features.geometryShader = VK_TRUE;
            }
            if (polygon && ((VkPhysicalDeviceFeatures2*)copy)->features.fillModeNonSolid) {
                ((VkPhysicalDeviceFeatures2*)copy)->features.fillModeNonSolid=VK_FALSE;
                ((VkPhysicalDeviceFeatures2*)copy)->features.drawIndirectFirstInstance=VK_TRUE;
            }
        }
        if (node == hdr_node) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
                ((VkPhysicalDeviceVulkan13Features *)copy)->textureCompressionASTC_HDR = VK_TRUE;
            else ((VkPhysicalDeviceTextureCompressionASTCHDRFeatures *)copy)->textureCompressionASTC_HDR = VK_TRUE;
        }
        if (node == target) {
            copy->pNext = (void *)out->suffix;
            break;
        }
    }
    out->info.pNext = out->copies;
    if (hdr && !hdr_node) {
        out->hdr = (VkPhysicalDeviceTextureCompressionASTCHDRFeatures){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES,
            .pNext = (void *)out->info.pNext, .textureCompressionASTC_HDR = VK_TRUE};
        out->info.pNext = &out->hdr;
    }
    return VK_SUCCESS;
}
