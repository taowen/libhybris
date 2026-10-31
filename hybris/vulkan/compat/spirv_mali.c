/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Adapted from MaliBridge 95a1e600a51299294e06bb09db2db8cc509fe6a3,
 * server/server_spvfix.c, and taowen/libhybris PR #2.
 */
#define VK_NO_PROTOTYPES
#include "spirv_mali.h"
#include "scaled_vertex.h"
#include "../layer/layer.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t once=PTHREAD_ONCE_INIT;
static int mode;
static void configure(void)
{
    if(getauxval(AT_SECURE))return;
    const char *value=getenv("HYBRIS_MALI_SPEC_CONSTANTS");
    mode=value && !strcmp(value,"0")?0:value && !strcmp(value,"force")?2:1;
}
int hybris_mali_spec_enabled(void)
{ pthread_once(&once,configure);return mode!=0; }
int hybris_mali_spec_active(VkPhysicalDevice physical)
{
    if(!hybris_mali_spec_enabled())return 0;
    if(mode==2)return 1;
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    PFN_vkGetPhysicalDeviceProperties query=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties properties;
    query(physical,&properties);
    return properties.vendorID==0x13b5; /* Arm; no application-name matching. */
}

VkResult hybris_spirv_mali_spec_constants(uint32_t *code,size_t bytes,
    const VkAllocationCallbacks *allocator,unsigned *changed)
{
    *changed=0;
    size_t words=bytes/sizeof(*code);
    if(bytes%sizeof(*code) || words<5 || code[0]!=0x07230203 || !code[3] || code[3]>(1u<<24))return VK_ERROR_UNKNOWN;
    /* Constants are declared before the first function, in dependency order.
     * Check framing before changing any instruction or indexing the ID table. */
    int candidate=0,has_specialization=0;
    for(size_t at=5;at<words;) {
        uint32_t op=code[at]&0xffff,length=code[at]>>16;
        if(!length || length>words-at)return VK_ERROR_UNKNOWN;
        if(op==54)break;
        candidate|=op==44;
        has_specialization|=op>=48 && op<=52;
        at+=length;
    }
    if(!candidate || !has_specialization)return VK_SUCCESS;
    uint8_t *specialized=hybris_scaled_alloc(allocator,code[3],VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if(!specialized)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(specialized,0,code[3]);
    VkResult result=VK_SUCCESS;
    for(size_t at=5;at<words;) {
        uint32_t op=code[at]&0xffff,length=code[at]>>16;
        if(op==54)break;
        if((op>=48 && op<=52) || op==44) {
            if(length<3 || code[at+2]>=code[3]){result=VK_ERROR_UNKNOWN;break;}
            uint32_t id=code[at+2];
            if(op!=44)specialized[id]=1;
            else {
                int depends=0;
                for(uint32_t i=3;i<length;++i) {
                    if(code[at+i]>=code[3]){result=VK_ERROR_UNKNOWN;break;}
                    depends|=specialized[code[at+i]];
                }
                if(result!=VK_SUCCESS)break;
                if(depends) {
                    code[at]=(length<<16)|51; /* OpSpecConstantComposite */
                    specialized[id]=1;++*changed;
                }
            }
        }
        at+=length;
    }
    hybris_scaled_free(allocator,specialized);
    return result;
}
