/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Adapted from MaliBridge 95a1e600a51299294e06bb09db2db8cc509fe6a3,
 * server/server_emu_compat.c, server/server_emu.c and client/emu.c.
 * Integration modified here. Split draws do not preserve shader DrawIndex.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "multidraw.h"
#include "../layer/layer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/auxv.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int policy=1;
static _Atomic unsigned reports;
static void configure(void)
{
    const char *value=getauxval(AT_SECURE)?NULL:getenv("HYBRIS_MULTI_DRAW_INDIRECT");
    if(value && !strcmp(value,"0"))policy=0;
    else if(value && !strcmp(value,"force"))policy=2;
}
int hybris_multidraw_active(VkPhysicalDevice physical)
{
    pthread_once(&once,configure);
    if(!policy)return 0;
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    PFN_vkGetPhysicalDeviceFeatures query=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures");
    if(!query)return 0;
    VkPhysicalDeviceFeatures native={0};query(physical,&native);
    return policy==2 || !native.multiDrawIndirect;
}
int hybris_multidraw_requested(const VkDeviceCreateInfo *info)
{
    VkBool32 requested=info->pEnabledFeatures?info->pEnabledFeatures->multiDrawIndirect:VK_FALSE;
    for(const VkBaseInStructure *node=info->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            requested=((const VkPhysicalDeviceFeatures2*)node)->features.multiDrawIndirect;
    return requested;
}
static void report(unsigned indexed,uint32_t count,uint32_t stride)
{
    if(!(atomic_fetch_or(&reports,1u<<indexed)&(1u<<indexed)))
        fprintf(stderr,"HYBRIS_MULTI_DRAW_INDIRECT indexed=%u count=%u stride=%u split=1 (DrawIndex not emulated)\n",indexed,count,stride);
}
static void VKAPI_CALL draw(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride)
{
    struct hybris_layer_device device;
    if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdDrawIndirect next=(void*)device.resolver(device.handle,"vkCmdDrawIndirect");
    if(count<=1){next(command,buffer,offset,count,stride);return;}
    report(0,count,stride);
    for(uint32_t i=0;i<count;++i)next(command,buffer,offset+(VkDeviceSize)i*stride,1,stride);
}
static void VKAPI_CALL draw_indexed(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride)
{
    struct hybris_layer_device device;
    if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdDrawIndexedIndirect next=(void*)device.resolver(device.handle,"vkCmdDrawIndexedIndirect");
    if(count<=1){next(command,buffer,offset,count,stride);return;}
    report(1,count,stride);
    for(uint32_t i=0;i<count;++i)next(command,buffer,offset+(VkDeviceSize)i*stride,1,stride);
}
PFN_vkVoidFunction hybris_multidraw_proc(const char *name)
{
    if(!strcmp(name,"vkCmdDrawIndirect"))return (PFN_vkVoidFunction)draw;
    if(!strcmp(name,"vkCmdDrawIndexedIndirect"))return (PFN_vkVoidFunction)draw_indexed;
    return NULL;
}
