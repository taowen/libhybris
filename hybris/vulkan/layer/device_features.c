/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "../compat/dual_source.h"
#include "../compat/multidraw.h"
#include "../compat/multiviewport.h"
#include "device_features.h"
#include "../compat/vertex_stores.h"
#include "../compat/clip_distance.h"
#include "../compat/bc_policy.h"
#include "../compat/scaled_vertex.h"
#include "../compat/shader_dispatch.h"
#include "../compat/robustness2.h"
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
    struct hybris_robustness2_policy robust;
    VkResult policy_result=hybris_robustness2_policy(physical,&robust);
    if(policy_result!=VK_SUCCESS)return policy_result;
    const VkBaseInStructure *robust_node=NULL, *image_node=NULL;
    unsigned requested_robust=0;
    for(const VkBaseInStructure *node=info->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            const VkPhysicalDeviceRobustness2FeaturesEXT *r=(const void*)node;
            robust_node=node;
            requested_robust=(r->robustBufferAccess2?HYBRIS_ROBUST_BUFFER:0) |
                (r->robustImageAccess2?HYBRIS_ROBUST_IMAGE:0) | (r->nullDescriptor?HYBRIS_ROBUST_NULL:0);
        }
    unsigned compatibility=requested_robust & robust.emulated;
    int force_buffer=(compatibility & (HYBRIS_ROBUST_BUFFER|HYBRIS_ROBUST_NULL))!=0;
    int force_image=(compatibility & (HYBRIS_ROBUST_IMAGE|HYBRIS_ROBUST_NULL)) && robust.core_image;
    if(robust.emulated & HYBRIS_ROBUST_NULL)
        for(uint32_t i=0;i<info->enabledExtensionCount;++i) {
            const char *name=info->ppEnabledExtensionNames[i];
            if(!strcmp(name,"VK_EXT_descriptor_buffer") || !strcmp(name,"VK_EXT_shader_object") ||
               !strcmp(name,"VK_KHR_acceleration_structure") || !strcmp(name,"VK_KHR_ray_tracing_pipeline") ||
               !strcmp(name,"VK_KHR_ray_query"))return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    if(robust.emulated && !robust.native_extensions && info->enabledExtensionCount) {
        out->extensions=hybris_scaled_alloc(allocator,(info->enabledExtensionCount+1)*sizeof(*out->extensions),VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if(!out->extensions)return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t count=0;
        for(uint32_t i=0;i<info->enabledExtensionCount;++i) {
            const char *name=info->ppEnabledExtensionNames[i];
            if(!strcmp(name,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) || !strcmp(name,VK_KHR_ROBUSTNESS_2_EXTENSION_NAME))continue;
            out->extensions[count++]=name;
        }
        out->info.ppEnabledExtensionNames=out->extensions;
        out->info.enabledExtensionCount=count;
    }
    if(force_image && robust.image_extension) {
        int present=0;
        for(uint32_t i=0;i<out->info.enabledExtensionCount;++i)
            present|=!strcmp(out->info.ppEnabledExtensionNames[i],VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
        if(!present) {
            const char **names=hybris_scaled_alloc(allocator,(out->info.enabledExtensionCount+1)*sizeof(*names),VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if(!names){hybris_device_features_release(out,allocator);return VK_ERROR_OUT_OF_HOST_MEMORY;}
            if(out->info.enabledExtensionCount)memcpy(names,out->info.ppEnabledExtensionNames,out->info.enabledExtensionCount*sizeof(*names));
            names[out->info.enabledExtensionCount++]=VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME;
            hybris_scaled_free(allocator,out->extensions);out->extensions=names;out->info.ppEnabledExtensionNames=names;
        }
    }
    int dual_src=hybris_dual_source_active(physical);
    int multi_draw=hybris_multidraw_active(physical);
    int multi_viewport=hybris_multiviewport_active(physical);
    int clip = hybris_clip_active(physical);
    int stores = hybris_vertex_stores_active(physical);
    int bc = hybris_bc_complete(physical);
    VkPhysicalDeviceFeatures compression = {0};
    hybris_bc_enable_transcode_features(physical, &compression);
    int transcode = compression.textureCompressionETC2 || compression.textureCompressionASTC_LDR;
    int hdr = hybris_bc_astc_hdr(physical);
    int cull = hybris_cull_active(physical);
    int stage_point = hybris_stage_point_active(physical);
    int polygon=hybris_shader_polygon_emulated(physical);
    if (!dual_src && !multi_draw && !multi_viewport && !clip && !stores && !bc && !cull && !stage_point && !polygon && !transcode && !hdr && !(robust_node && robust.emulated)) return VK_SUCCESS;
    if (info->pEnabledFeatures) {
        out->features = *info->pEnabledFeatures;
        if(dual_src)out->features.dualSrcBlend=VK_FALSE;
        if(multi_draw)out->features.multiDrawIndirect=VK_FALSE;
        if(multi_viewport)out->features.multiViewport=VK_FALSE;
        if(force_buffer)out->features.robustBufferAccess=VK_TRUE;
        if (transcode) hybris_bc_enable_transcode_features(physical, &out->features);
        if (clip) out->features.shaderClipDistance = VK_FALSE;
        if (stores) out->features.vertexPipelineStoresAndAtomics = VK_FALSE;
        if (bc) out->features.textureCompressionBC = VK_FALSE;
        if (cull) out->features.shaderCullDistance = VK_FALSE;
        if (stage_point) out->features.shaderTessellationAndGeometryPointSize = VK_FALSE;
        if (polygon && out->features.fillModeNonSolid) {
            out->features.fillModeNonSolid=VK_FALSE;
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
        if(node==robust_node && robust.emulated)target=node;
        if(force_image && (node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES ||
                          node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
            image_node=node;target=node;
        }
    }
    if (!features2 && !info->pEnabledFeatures && (cull || transcode || force_buffer)) {
        out->features = (VkPhysicalDeviceFeatures){.geometryShader = cull ? VK_TRUE : VK_FALSE};
        if (transcode) hybris_bc_enable_transcode_features(physical, &out->features);
        if(force_buffer)out->features.robustBufferAccess=VK_TRUE;
        out->info.pEnabledFeatures = &out->features;
    }
    if (hdr == 2) {
        int present = 0;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            present |= !strcmp(info->ppEnabledExtensionNames[i], VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);
        if (!present) {
            const char **names=hybris_scaled_alloc(allocator,
                (out->info.enabledExtensionCount+1)*sizeof(*names),VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if(!names){hybris_device_features_release(out,allocator);return VK_ERROR_OUT_OF_HOST_MEMORY;}
            if(out->info.enabledExtensionCount)memcpy(names,out->info.ppEnabledExtensionNames,out->info.enabledExtensionCount*sizeof(*names));
            names[out->info.enabledExtensionCount]=VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME;
            hybris_scaled_free(allocator,out->extensions);out->extensions=names;
            out->info.ppEnabledExtensionNames=names;
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
        if(node==robust_node && robust.emulated && !robust.native_extensions) {
            if(node==target){*slot=(void*)out->suffix;break;}
            continue;
        }
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
            if(dual_src)((VkPhysicalDeviceFeatures2*)copy)->features.dualSrcBlend=VK_FALSE;
            if(multi_draw)((VkPhysicalDeviceFeatures2*)copy)->features.multiDrawIndirect=VK_FALSE;
            if(multi_viewport)((VkPhysicalDeviceFeatures2*)copy)->features.multiViewport=VK_FALSE;
            if(force_buffer)((VkPhysicalDeviceFeatures2*)copy)->features.robustBufferAccess=VK_TRUE;
            if (transcode) hybris_bc_enable_transcode_features(physical, &((VkPhysicalDeviceFeatures2 *)copy)->features);
            if (clip) ((VkPhysicalDeviceFeatures2 *)copy)->features.shaderClipDistance = VK_FALSE;
            if (stores) ((VkPhysicalDeviceFeatures2 *)copy)->features.vertexPipelineStoresAndAtomics = VK_FALSE;
            if (bc) ((VkPhysicalDeviceFeatures2 *)copy)->features.textureCompressionBC = VK_FALSE;
            if (cull) ((VkPhysicalDeviceFeatures2 *)copy)->features.shaderCullDistance = VK_FALSE;
            if (stage_point) ((VkPhysicalDeviceFeatures2 *)copy)->features.shaderTessellationAndGeometryPointSize = VK_FALSE;
            if (polygon && ((VkPhysicalDeviceFeatures2*)copy)->features.fillModeNonSolid) {
                ((VkPhysicalDeviceFeatures2*)copy)->features.fillModeNonSolid=VK_FALSE;
            }
        }
        if (node == hdr_node) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
                ((VkPhysicalDeviceVulkan13Features *)copy)->textureCompressionASTC_HDR = VK_TRUE;
            else ((VkPhysicalDeviceTextureCompressionASTCHDRFeatures *)copy)->textureCompressionASTC_HDR = VK_TRUE;
        }
        if(node==robust_node && robust.emulated) {
            VkPhysicalDeviceRobustness2FeaturesEXT *r=(void*)copy;
            if(robust.emulated & HYBRIS_ROBUST_BUFFER)r->robustBufferAccess2=VK_FALSE;
            if(robust.emulated & HYBRIS_ROBUST_IMAGE)r->robustImageAccess2=VK_FALSE;
            if(robust.emulated & HYBRIS_ROBUST_NULL)r->nullDescriptor=VK_FALSE;
        }
        if(node==image_node) {
            if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
                ((VkPhysicalDeviceVulkan13Features*)copy)->robustImageAccess=VK_TRUE;
            else ((VkPhysicalDeviceImageRobustnessFeatures*)copy)->robustImageAccess=VK_TRUE;
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
    if(force_image && !image_node) {
        out->image_robustness=(VkPhysicalDeviceImageRobustnessFeatures){
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES,
            .pNext=(void*)out->info.pNext,.robustImageAccess=VK_TRUE};
        out->info.pNext=&out->image_robustness;
    }
    if(compatibility)fprintf(stderr,"HYBRIS_ROBUSTNESS2 compatibility=0x%x core_buffer=%d core_image=%d\n",compatibility,force_buffer,force_image);
    return VK_SUCCESS;
}
