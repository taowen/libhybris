/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "vertex_divisor.h"
#include "scaled_vertex.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

/* A zero divisor with firstInstance == 0 is exactly a zero-stride fetch.
 * Keep native divisors unchanged. The KHR property explicitly limits nonzero
 * firstInstance while this fallback is active, including indirect draws. */
struct hybris_divisor_policy hybris_divisor_policy(VkPhysicalDevice physical)
{
    struct hybris_divisor_policy result={0};
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return result;
    PFN_vkEnumerateDeviceExtensionProperties enumerate=(void*)owner.resolver(owner.instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t count=0;
    if(enumerate(physical,NULL,&count,NULL)!=VK_SUCCESS)return result;
    VkExtensionProperties *extensions=calloc(count,sizeof(*extensions));
    if(!extensions)return result;
    int ext=0,khr=0;
    if(enumerate(physical,NULL,&count,extensions)==VK_SUCCESS)
        for(uint32_t i=0;i<count;++i) {
            ext|=!strcmp(extensions[i].extensionName,VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
            khr|=!strcmp(extensions[i].extensionName,VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        }
    free(extensions);
    if(!khr)return result;
    PFN_vkGetPhysicalDeviceFeatures2 features=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR divisor={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&divisor};
    features(physical,&f);
    if(!divisor.vertexAttributeInstanceRateDivisor)return result;
    VkPhysicalDeviceProperties properties;
    ((PFN_vkGetPhysicalDeviceProperties)owner.resolver(owner.instance,"vkGetPhysicalDeviceProperties"))(physical,&properties);
    /* Command storage covers every advertised binding on this backend. */
    if(properties.limits.maxVertexInputBindings>HYBRIS_DIVISOR_BINDINGS)return result;
    result.alias=!ext;
    result.zero=!divisor.vertexAttributeInstanceRateZeroDivisor;
    return result;
}

VkResult hybris_divisor_pipeline_prepare(VkGraphicsPipelineCreateInfo *info,
    struct hybris_divisor_pipeline *copy,const VkAllocationCallbacks *allocator)
{
    const VkPipelineVertexInputStateCreateInfo *input=info->pVertexInputState;
    if(info->pDynamicState)for(uint32_t i=0;i<info->pDynamicState->dynamicStateCount;++i)
        copy->dynamic|=info->pDynamicState->pDynamicStates[i]==VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if(!input)return VK_SUCCESS;
    const VkPipelineVertexInputDivisorStateCreateInfoEXT *divisor=NULL;
    for(const VkBaseInStructure *n=input->pNext;n;n=n->pNext)
        if(n->sType==VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT)divisor=(const void*)n;
    if(!divisor)return VK_SUCCESS;
    for(uint32_t i=0;i<divisor->vertexBindingDivisorCount;++i)
        if(!divisor->pVertexBindingDivisors[i].divisor) {
            uint32_t binding=divisor->pVertexBindingDivisors[i].binding;
            if(binding>=HYBRIS_DIVISOR_BINDINGS)return VK_ERROR_FEATURE_NOT_PRESENT;
            copy->mask|=1u<<binding;
        }
    if(!copy->mask)return VK_SUCCESS;
    /* Preserve unknown chains rather than removing application state. The
     * public policy disables dynamic vertex inputs and pipeline libraries. */
    if(input->pNext!=divisor || divisor->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;
    copy->bindings=hybris_scaled_alloc(allocator,input->vertexBindingDescriptionCount*sizeof(*copy->bindings),VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    copy->divisors=hybris_scaled_alloc(allocator,divisor->vertexBindingDivisorCount*sizeof(*copy->divisors),VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if(!copy->bindings || !copy->divisors)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy->bindings,input->pVertexBindingDescriptions,input->vertexBindingDescriptionCount*sizeof(*copy->bindings));
    memcpy(copy->divisors,divisor->pVertexBindingDivisors,divisor->vertexBindingDivisorCount*sizeof(*copy->divisors));
    for(uint32_t i=0;i<input->vertexBindingDescriptionCount;++i)
        if(copy->bindings[i].binding<32 && (copy->mask&(1u<<copy->bindings[i].binding)))copy->bindings[i].stride=0;
    for(uint32_t i=0;i<divisor->vertexBindingDivisorCount;++i)
        if(!copy->divisors[i].divisor)copy->divisors[i].divisor=1;
    if(info->pDynamicState)for(uint32_t i=0;i<info->pDynamicState->dynamicStateCount;++i)
        copy->dynamic|=info->pDynamicState->pDynamicStates[i]==VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    copy->divisor=*divisor;copy->divisor.pVertexBindingDivisors=copy->divisors;
    copy->input=*input;copy->input.pVertexBindingDescriptions=copy->bindings;copy->input.pNext=&copy->divisor;
    info->pVertexInputState=&copy->input;
    return VK_SUCCESS;
}
void hybris_divisor_pipeline_release(struct hybris_divisor_pipeline *copy,const VkAllocationCallbacks *allocator)
{hybris_scaled_free(allocator,copy->bindings);hybris_scaled_free(allocator,copy->divisors);}
struct pipeline {
    VkDevice device;VkPipeline handle;uint32_t mask;int dynamic,custom;
    VkAllocationCallbacks allocator;struct pipeline *next;
};
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
#define PIPELINE_BUCKETS 1024
static struct pipeline *pipelines[PIPELINE_BUCKETS];
static size_t bucket(VkDevice device,VkPipeline handle)
{
    uint64_t key=(uint64_t)(uintptr_t)handle ^ ((uint64_t)(uintptr_t)device>>4);
    key^=key>>33;key*=UINT64_C(0xff51afd7ed558ccd);key^=key>>33;
    return key&(PIPELINE_BUCKETS-1);
}
VkResult hybris_divisor_pipeline_publish(VkDevice device,VkPipeline handle,
    const struct hybris_divisor_pipeline *copy,const VkAllocationCallbacks *allocator)
{
    if(!copy->mask && !copy->dynamic)return VK_SUCCESS;
    struct pipeline *p=hybris_scaled_alloc(allocator,sizeof(*p),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    *p=(struct pipeline){.device=device,.handle=handle,.mask=copy->mask,.dynamic=copy->dynamic,.custom=allocator!=NULL};
    if(allocator)p->allocator=*allocator;
    size_t index=bucket(device,handle);
    pthread_mutex_lock(&guard);p->next=pipelines[index];pipelines[index]=p;pthread_mutex_unlock(&guard);
    return VK_SUCCESS;
}
void hybris_divisor_pipeline_remove(VkDevice device,VkPipeline handle)
{
    struct pipeline *p=NULL;
    pthread_mutex_lock(&guard);
    for(struct pipeline **slot=&pipelines[bucket(device,handle)];*slot;slot=&(*slot)->next)
        if((*slot)->device==device && (*slot)->handle==handle){p=*slot;*slot=p->next;break;}
    pthread_mutex_unlock(&guard);
    if(p)hybris_scaled_free(p->custom?&p->allocator:NULL,p);
}
void hybris_divisor_device_release(VkDevice device)
{
    pthread_mutex_lock(&guard);
    for(size_t i=0;i<PIPELINE_BUCKETS;++i)for(struct pipeline **slot=&pipelines[i];*slot;)
        if((*slot)->device==device){struct pipeline *p=*slot;*slot=p->next;hybris_scaled_free(p->custom?&p->allocator:NULL,p);}
        else slot=&(*slot)->next;
    pthread_mutex_unlock(&guard);
}
void hybris_divisor_vertices(struct hybris_divisor_recording *s,uint32_t first,uint32_t count,
    const VkBuffer *buffers,const VkDeviceSize *offsets,const VkDeviceSize *sizes,const VkDeviceSize *strides)
{
    for(uint32_t i=0;i<count && first+i<HYBRIS_DIVISOR_BINDINGS;++i) {
        uint32_t b=first+i;s->buffers[b]=buffers[i];s->offsets[b]=offsets[i];
        s->sizes[b]=sizes?sizes[i]:VK_WHOLE_SIZE;s->buffers_known|=1u<<b;
        if(strides){s->strides[b]=strides[i];s->strides_known|=1u<<b;}
    }
}
const VkDeviceSize *hybris_divisor_strides(const struct hybris_divisor_recording *s,
    uint32_t first,uint32_t count,const VkDeviceSize *strides,VkDeviceSize *out)
{
    if(!strides || !s->dynamic || !s->zero)return strides;
    if(first>=32 || count>32-first)return strides;
    for(uint32_t i=0;i<count;++i)out[i]=(s->zero&(1u<<(first+i)))?0:strides[i];
    return out;
}
void hybris_divisor_bind(VkCommandBuffer command,const struct hybris_layer_device *device,VkPipeline handle)
{
    if(!device->zero_divisor)return;
    struct hybris_divisor_recording *s=hybris_layer_divisor_recording(command);
    if(!s)return;
    s->zero=0;s->dynamic=0;
    pthread_mutex_lock(&guard);
    for(struct pipeline *p=pipelines[bucket(device->handle,handle)];p;p=p->next)
        if(p->device==device->handle && p->handle==handle){s->zero=p->mask;s->dynamic=p->dynamic;break;}
    pthread_mutex_unlock(&guard);
    /* Restore app strides on transitions between dynamic pipelines. Static
     * pipelines own their baked strides and must not receive dynamic writes. */
    if(!s->dynamic)return;
    uint32_t changed=(s->overridden|s->zero)&s->strides_known&s->buffers_known;
    s->overridden=s->zero;
    if(!changed)return;
    PFN_vkCmdBindVertexBuffers2 bind=device->recording->CmdBindVertexBuffers2;
    if(!bind)bind=device->recording->CmdBindVertexBuffers2EXT;
    if(!bind){hybris_layer_command_error(command,VK_ERROR_FEATURE_NOT_PRESENT);return;}
    for(uint32_t b=0;b<32;++b)if(changed&(1u<<b)) {
        VkDeviceSize stride=(s->zero&(1u<<b))?0:s->strides[b];
        bind(command,b,1,&s->buffers[b],&s->offsets[b],&s->sizes[b],&stride);
    }
}

int hybris_divisor_draw(VkCommandBuffer command,const struct hybris_layer_device *device,uint32_t first_instance)
{
    if(!device->zero_divisor || !first_instance)return 1;
    const struct hybris_divisor_recording *s=hybris_layer_divisor_recording(command);
    if(!s || !s->zero)return 1;
    /* Reject usage outside the advertised KHR limit, never silently fetch the
     * wrong vertex. Valid indirect draws obey the same firstInstance limit. */
    hybris_layer_command_error(command,VK_ERROR_FEATURE_NOT_PRESENT);
    return 0;
}
