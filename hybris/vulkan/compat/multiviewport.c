/* SPDX-License-Identifier: GPL-3.0-or-later
 * First-viewport approximation adapted from MaliBridge by ASEbrahim and contributors:
 * 95a1e600a51299294e06bb09db2db8cc509fe6a3, server/server_emu_compat.c and client/emu.c.
 * This does not implement shader ViewportIndex or true multiple viewport rendering.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "multiviewport.h"
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
    const char *value=getauxval(AT_SECURE)?NULL:getenv("HYBRIS_MULTI_VIEWPORT");
    if(value && !strcmp(value,"0"))policy=0;
    else if(value && !strcmp(value,"force"))policy=2;
}
int hybris_multiviewport_active(VkPhysicalDevice physical)
{
    pthread_once(&once,configure);
    if(!policy)return 0;
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    PFN_vkGetPhysicalDeviceFeatures query=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures");
    if(!query)return 0;
    VkPhysicalDeviceFeatures native={0};query(physical,&native);
    return policy==2 || !native.multiViewport;
}
int hybris_multiviewport_requested(const VkDeviceCreateInfo *info)
{
    VkBool32 requested=info->pEnabledFeatures?info->pEnabledFeatures->multiViewport:VK_FALSE;
    for(const VkBaseInStructure *node=info->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            requested=((const VkPhysicalDeviceFeatures2*)node)->features.multiViewport;
    return requested;
}
uint32_t hybris_multiviewport_count(unsigned category,uint32_t first,uint32_t count)
{
    uint32_t retained=first?0:(count>1?1:count);
    if(count!=retained && !(atomic_fetch_or(&reports,1u<<category)&(1u<<category)))
        fprintf(stderr,"HYBRIS_MULTI_VIEWPORT category=%u first=%u count=%u retained=%u (first-viewport approximation)\n",category,first,count,retained);
    return retained;
}
