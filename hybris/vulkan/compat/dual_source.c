/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Adapted from MaliBridge 95a1e600a51299294e06bb09db2db8cc509fe6a3,
 * server/server_emu_compat.c and server/server_emu_shader.c.
 * Modified integration: reserve an unused output location within device limits;
 * fail unsupported interfaces instead of clamping into another live output.
 * This substitutes source zero for source one, not exact dual-source blending.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "dual_source.h"
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
    const char *value=getauxval(AT_SECURE)?NULL:getenv("HYBRIS_DUAL_SOURCE_BLEND");
    if(value && !strcmp(value,"0"))policy=0;
    else if(value && !strcmp(value,"force"))policy=2;
}
int hybris_dual_source_active(VkPhysicalDevice physical)
{
    pthread_once(&once,configure);
    if(!policy)return 0;
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    PFN_vkGetPhysicalDeviceFeatures query=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures");
    if(!query)return 0;
    VkPhysicalDeviceFeatures native={0};query(physical,&native);
    return policy==2 || !native.dualSrcBlend;
}
int hybris_dual_source_requested(const VkDeviceCreateInfo *info)
{
    VkBool32 enabled=info->pEnabledFeatures?info->pEnabledFeatures->dualSrcBlend:VK_FALSE;
    for(const VkBaseInStructure *node=info->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)enabled=((const VkPhysicalDeviceFeatures2*)node)->features.dualSrcBlend;
    return enabled;
}
static void note(unsigned category)
{
    if(!(atomic_fetch_or(&reports,1u<<category)&(1u<<category)))
        fprintf(stderr,"HYBRIS_DUAL_SOURCE_BLEND category=%u source1 replaced by source0; blending is approximate\n",category);
}
VkBlendFactor hybris_dual_source_factor(VkBlendFactor factor)
{
    VkBlendFactor replacement;
    switch(factor) {
    case VK_BLEND_FACTOR_SRC1_COLOR:replacement=VK_BLEND_FACTOR_SRC_COLOR;break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:replacement=VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;break;
    case VK_BLEND_FACTOR_SRC1_ALPHA:replacement=VK_BLEND_FACTOR_SRC_ALPHA;break;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:replacement=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;break;
    default:return factor;
    }
    note(0);return replacement;
}
/* Framing is checked before any scan or modification of the private module. */
int hybris_dual_source_outputs(const uint32_t *code,size_t bytes)
{
    if(bytes%4 || bytes<20 || code[0]!=0x07230203)return -1;
    int found=0;
    for(size_t at=5;at<bytes/4;) {
        unsigned n=code[at]>>16,op=code[at]&0xffff;
        if(!n||n>bytes/4-at)return -1;
        if(op==71 && n>=4 && code[at+2]==32 && code[at+3])found=1;
        if(op==72 && n>=5 && code[at+3]==32 && code[at+4])return -1;
        at+=n;
    }
    return found;
}
static uint32_t output_type(const uint32_t *code,size_t words,uint32_t id)
{
    for(size_t at=5;at<words;at+=code[at]>>16)
        if((code[at]&0xffff)==59 && (code[at]>>16)>=4 && code[at+2]==id && code[at+3]==3)return code[at+1];
    return 0;
}
static int simple_output(const uint32_t *code,size_t words,uint32_t pointer)
{
    uint32_t type=0;
    for(size_t at=5;at<words;at+=code[at]>>16)
        if((code[at]&0xffff)==32 && (code[at]>>16)>=4 && code[at+1]==pointer)type=code[at+3];
    for(size_t at=5;at<words;at+=code[at]>>16)
        if((code[at]&0xffff)==23 && (code[at]>>16)==4 && code[at+1]==type) {
            if(code[at+3]>4)return 0;
            type=code[at+2];break;
        }
    for(size_t at=5;at<words;at+=code[at]>>16) {
        unsigned op=code[at]&0xffff,n=code[at]>>16;
        if((op==21 || op==22) && n>=3 && code[at+1]==type)return code[at+2]==32;
    }
    return 0;
}
VkResult hybris_dual_source_spirv(uint32_t *code,size_t bytes,uint32_t attachments,uint32_t limit)
{
    int found=hybris_dual_source_outputs(code,bytes);
    if(found<0)return VK_ERROR_FEATURE_NOT_PRESENT;
    if(!found)return VK_SUCCESS;
    size_t words=bytes/4;
    for(size_t at=5;at<words;at+=code[at]>>16) {
        unsigned op=code[at]&0xffff,n=code[at]>>16;
        if(op==74 || op==75 || (op==72 && n>=5 && code[at+3]==30))return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Plain scalar/vector output variables only; arrays, interface blocks and
     * grouped decorations need interface analysis beyond this approximation. */
    for(size_t at=5;at<words;at+=code[at]>>16)
        if((code[at]&0xffff)==71 && (code[at]>>16)>=4 && code[at+2]==30) {
            uint32_t type=output_type(code,words,code[at+1]);
            if(type && !simple_output(code,words,type))return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    for(size_t at=5;at<words;at+=code[at]>>16) {
        if((code[at]&0xffff)!=71 || (code[at]>>16)<4 || code[at+2]!=32 || !code[at+3])continue;
        uint32_t id=code[at+1];
        if(code[at+3]!=1 || !output_type(code,words,id))return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t free_location=attachments?attachments:1;
        for(;free_location<limit;++free_location) {
            int occupied=0;
            for(size_t scan=5;scan<words;scan+=code[scan]>>16)
                if((code[scan]&0xffff)==71 && (code[scan]>>16)>=4 && code[scan+2]==30 && code[scan+3]==free_location && output_type(code,words,code[scan+1]))occupied=1;
            if(!occupied)break;
        }
        if(free_location>=limit)return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t *location=NULL;
        for(size_t scan=5;scan<words;scan+=code[scan]>>16)
            if((code[scan]&0xffff)==71 && (code[scan]>>16)>=4 && code[scan+1]==id && code[scan+2]==30)location=&code[scan+3];
        if(!location)return VK_ERROR_FEATURE_NOT_PRESENT;
        *location=free_location;code[at+3]=0;note(1);
    }
    return VK_SUCCESS;
}
