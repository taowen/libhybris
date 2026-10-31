/* SPDX-License-Identifier: GPL-3.0-or-later
 * MaliBridge 95a1e600 prepass idea, integrated with existing submit ownership.
 * Waits cover compute; each app command is preceded by its own index prepasses.
 */
#define VK_NO_PROTOTYPES
#include "polygon_record.h"
#include "../layer/layer.h"
#include <string.h>
struct block {struct block *next;max_align_t alignment;};
struct arena {const VkAllocationCallbacks *allocator;struct block *blocks;};
static void *alloc(struct arena *a,size_t count,size_t size)
{
    if(size && count>(SIZE_MAX-sizeof(struct block))/size)return NULL;
    struct block *b=hybris_scaled_alloc(a->allocator,sizeof(*b)+count*size,VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);if(!b)return NULL;
    b->next=a->blocks;a->blocks=b;void *p=b+1;memset(p,0,count*size);return p;
}
static void finish(struct arena *a){while(a->blocks){struct block *next=a->blocks->next;hybris_scaled_free(a->allocator,a->blocks);a->blocks=next;}}
static VkCommandBuffer *expand(struct arena *a,uint32_t n,const VkCommandBuffer *commands,uint32_t *total)
{
    uint64_t length=n;
    for(uint32_t i=0;i<n;++i){uint32_t extra=hybris_polygon_record_prepasses(commands[i],NULL,0);if(extra==UINT32_MAX)return NULL;length+=extra;}
    if(length>UINT32_MAX)return NULL;*total=(uint32_t)length;
    if(length==n)return (VkCommandBuffer*)commands;
    VkCommandBuffer *list=alloc(a,(size_t)length,sizeof(*list));if(!list)return NULL;uint32_t used=0;
    for(uint32_t i=0;i<n;++i){uint32_t capacity=hybris_polygon_record_prepasses(commands[i],NULL,0),start=used;
        uint32_t got=hybris_polygon_record_prepasses(commands[i],list+used,capacity);if(got!=capacity)return NULL;
        for(uint32_t j=0;j<got;++j){VkCommandBuffer cb=list[start+j];int duplicate=0;for(uint32_t k=start;k<used;++k)duplicate|=list[k]==cb;if(!duplicate)list[used++]=cb;}
        list[used++]=commands[i];}
    *total=used;return list;
}
static size_t submit_node_size(VkStructureType type)
{
    switch(type){
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:return sizeof(VkDeviceGroupSubmitInfo);
    case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:return sizeof(VkTimelineSemaphoreSubmitInfo);
    case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO:return sizeof(VkProtectedSubmitInfo);
    case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR:return sizeof(VkPerformanceQuerySubmitInfoKHR);
    default:return 0;}
}
static VkResult group_chain(struct arena *a,const VkSubmitInfo *original,VkSubmitInfo *copy)
{
    const VkBaseInStructure *group=NULL;
    for(const VkBaseInStructure *n=original->pNext;n;n=n->pNext)if(n->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)group=n;
    if(!group)return VK_SUCCESS;
    VkBaseOutStructure *first=NULL,**slot=&first;
    for(const VkBaseInStructure *n=original->pNext;n;n=n->pNext){size_t size=submit_node_size(n->sType);if(!size){hybris_polygon_note("unsupported submit prefix before device-group node");return VK_ERROR_FEATURE_NOT_PRESENT;}
        VkBaseOutStructure *node=alloc(a,1,size);if(!node)return VK_ERROR_OUT_OF_HOST_MEMORY;memcpy(node,n,size);*slot=node;slot=&node->pNext;
        if(n==group){VkDeviceGroupSubmitInfo *g=(void*)node;const VkDeviceGroupSubmitInfo *old=(const void*)n;
            uint32_t *masks=alloc(a,copy->commandBufferCount,sizeof(*masks));if(!masks)return VK_ERROR_OUT_OF_HOST_MEMORY;
            uint32_t original_index=0;for(uint32_t i=0;i<copy->commandBufferCount;++i){masks[i]=old->commandBufferCount?old->pCommandBufferDeviceMasks[original_index]:1;if(copy->pCommandBuffers[i]==original->pCommandBuffers[original_index] && original_index+1<original->commandBufferCount)++original_index;}
            g->commandBufferCount=copy->commandBufferCount;g->pCommandBufferDeviceMasks=masks;break;}}
    copy->pNext=first;return VK_SUCCESS;
}
VkResult hybris_polygon_submit(VkQueue queue,uint32_t count,const VkSubmitInfo *info,VkFence fence,PFN_vkQueueSubmit next)
{
    int needed=0;
    for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<info[i].commandBufferCount;++j)if(hybris_polygon_record_prepasses(info[i].pCommandBuffers[j],NULL,0))needed=1;
    if(!needed)return next(queue,count,info,fence);
    struct hybris_layer_device device;if(!hybris_layer_lookup_queue(queue,&device))return VK_ERROR_INITIALIZATION_FAILED;
    VkAllocationCallbacks allocator;struct arena a={0};if(hybris_layer_device_allocator(device.handle,&allocator))a.allocator=&allocator;
    VkSubmitInfo *changed=alloc(&a,count,sizeof(*changed));VkResult result=VK_ERROR_OUT_OF_HOST_MEMORY;if(!changed)goto done;
    memcpy(changed,info,count*sizeof(*changed));
    for(uint32_t i=0;i<count;++i){uint32_t total=0;VkCommandBuffer *list=expand(&a,info[i].commandBufferCount,info[i].pCommandBuffers,&total);if(info[i].commandBufferCount&&!list)goto done;
        changed[i].pCommandBuffers=list;changed[i].commandBufferCount=total;
        if(total!=info[i].commandBufferCount){
            if(info[i].waitSemaphoreCount){VkPipelineStageFlags *stages=alloc(&a,info[i].waitSemaphoreCount,sizeof(*stages));if(!stages)goto done;for(uint32_t j=0;j<info[i].waitSemaphoreCount;++j)stages[j]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;changed[i].pWaitDstStageMask=stages;}
            VkResult chain=group_chain(&a,&info[i],&changed[i]);if(chain){result=chain;goto done;}
        }}
    result=next(queue,count,changed,fence);
done:finish(&a);return result;
}
VkResult hybris_polygon_submit2(VkQueue queue,uint32_t count,const VkSubmitInfo2 *info,VkFence fence,PFN_vkQueueSubmit2 next)
{
    int needed=0;for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<info[i].commandBufferInfoCount;++j)if(hybris_polygon_record_prepasses(info[i].pCommandBufferInfos[j].commandBuffer,NULL,0))needed=1;
    if(!needed)return next(queue,count,info,fence);
    struct hybris_layer_device device;if(!hybris_layer_lookup_queue(queue,&device))return VK_ERROR_INITIALIZATION_FAILED;
    VkAllocationCallbacks allocator;struct arena a={0};if(hybris_layer_device_allocator(device.handle,&allocator))a.allocator=&allocator;
    VkSubmitInfo2 *changed=alloc(&a,count,sizeof(*changed));VkResult result=VK_ERROR_OUT_OF_HOST_MEMORY;if(!changed)goto done;memcpy(changed,info,count*sizeof(*changed));
    for(uint32_t i=0;i<count;++i){uint32_t n=info[i].commandBufferInfoCount,total=0;VkCommandBuffer *original=alloc(&a,n,sizeof(*original));if(n&&!original)goto done;for(uint32_t j=0;j<n;++j)original[j]=info[i].pCommandBufferInfos[j].commandBuffer;
        VkCommandBuffer *list=expand(&a,n,original,&total);if(n&&!list)goto done;
        VkCommandBufferSubmitInfo *commands=alloc(&a,total,sizeof(*commands));if(total&&!commands)goto done;uint32_t original_index=0;
        for(uint32_t j=0;j<total;++j){if(list[j]==original[original_index]){commands[j]=info[i].pCommandBufferInfos[original_index];if(original_index+1<n)++original_index;}
            else commands[j]=(VkCommandBufferSubmitInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=list[j],.deviceMask=info[i].pCommandBufferInfos[original_index].deviceMask};}
        changed[i].pCommandBufferInfos=commands;changed[i].commandBufferInfoCount=total;
        if(total!=n && info[i].waitSemaphoreInfoCount){VkSemaphoreSubmitInfo *waits=alloc(&a,info[i].waitSemaphoreInfoCount,sizeof(*waits));if(!waits)goto done;memcpy(waits,info[i].pWaitSemaphoreInfos,info[i].waitSemaphoreInfoCount*sizeof(*waits));for(uint32_t j=0;j<info[i].waitSemaphoreInfoCount;++j)waits[j].stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;changed[i].pWaitSemaphoreInfos=waits;}}
    result=next(queue,count,changed,fence);
done:finish(&a);return result;
}
