/* SPDX-License-Identifier: GPL-3.0-or-later
 * Capability policy and misaligned-view diagnostics adapted from MaliBridge
 * 3cd7a2baaaf07f7d7a72c73675899a5ea202ebd9, common/vkb_caps.c and
 * server/server_emu.c, server/server_emu_layer.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors.
 */
#define VK_NO_PROTOTYPES
#include "texel_alignment.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

VkDeviceSize hybris_texel_alignment(VkPhysicalDevice physical)
{
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    VkPhysicalDeviceTexelBufferAlignmentProperties alignment={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,.pNext=&alignment};
    VkPhysicalDeviceProperties2 native={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};
    PFN_vkGetPhysicalDeviceProperties2 query=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceProperties2");
    if(!query)return 0;
    query(physical,&native);
    if(driver.driverID!=VK_DRIVER_ID_ARM_PROPRIETARY || native.properties.apiVersion<VK_API_VERSION_1_3 ||
        alignment.storageTexelBufferOffsetSingleTexelAlignment || alignment.storageTexelBufferOffsetAlignmentBytes<=1)return 0;
    return alignment.storageTexelBufferOffsetAlignmentBytes;
}
void hybris_texel_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *properties)
{
    if(!hybris_texel_alignment(physical))return;
    for(VkBaseOutStructure *node=properties->pNext;node;node=node->pNext) {
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES)
            ((VkPhysicalDeviceVulkan13Properties*)node)->storageTexelBufferOffsetSingleTexelAlignment=VK_TRUE;
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES)
            ((VkPhysicalDeviceTexelBufferAlignmentProperties*)node)->storageTexelBufferOffsetSingleTexelAlignment=VK_TRUE;
    }
}
static VkResult VKAPI_CALL create_view(VkDevice handle,const VkBufferViewCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkBufferView *out)
{
    struct hybris_layer_device device;
    if(!hybris_layer_lookup_device(handle,&device))return VK_ERROR_INITIALIZATION_FAILED;
    static _Atomic unsigned reports;
    VkDeviceSize alignment=device.storage_texel_alignment;
    if(alignment && info->offset%alignment && atomic_fetch_add(&reports,1)<8)
        fprintf(stderr,"HYBRIS_TEXEL_ALIGNMENT offset=%llu format=%u range=%llu native_alignment=%llu; storage texel alignment is not guaranteed\n",
            (unsigned long long)info->offset,info->format,(unsigned long long)info->range,(unsigned long long)alignment);
    return ((PFN_vkCreateBufferView)device.resolver(handle,"vkCreateBufferView"))(handle,info,allocator,out);
}
PFN_vkVoidFunction hybris_texel_proc(const char *name)
{return !strcmp(name,"vkCreateBufferView")?(PFN_vkVoidFunction)create_view:NULL;}
