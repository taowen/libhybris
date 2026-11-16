/* SPDX-License-Identifier: Apache-2.0
 * Identical BC aliases share their compressed backing and decoded allocation,
 * while retaining distinct native image handles and layouts. */
#define VK_NO_PROTOTYPES
#include "bc_alias.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define BUCKETS 1024
struct hybris_bc_alias {
    VkDevice device;
    PFN_vkGetDeviceProcAddr resolver;
    VkDeviceMemory backing, decoded;
    VkDeviceSize offset;
    VkImageCreateInfo info;
    VkMemoryRequirements requirements;
    VkFormat decoded_format;
    VkImageCompressionFlagsEXT compression;
    uint32_t memory_type, view_count;
    VkFormat *views;
    uint32_t *queues;
    VkBool32 dedicated, compression_control;
    size_t references;
    struct hybris_bc_alias *next, *memory_next;
};
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static struct hybris_bc_alias *index[BUCKETS];
static struct hybris_bc_alias *memory_index[BUCKETS];
static size_t bucket(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset)
{
    uint64_t key=(uintptr_t)device ^ (uint64_t)memory ^ offset;
    key^=key>>33;key*=UINT64_C(0xff51afd7ed558ccd);key^=key>>33;
    return key&(BUCKETS-1);
}
static void discard(struct hybris_bc_alias *r)
{free(r->views);free(r->queues);free(r);}
static int identical(const struct hybris_bc_alias *a,const struct hybris_bc_alias *b)
{
    const VkImageCreateInfo *x=&a->info,*y=&b->info;
    return a->decoded_format==b->decoded_format && a->memory_type==b->memory_type &&
        a->requirements.size==b->requirements.size && a->requirements.alignment==b->requirements.alignment &&
        a->compression_control==b->compression_control && a->compression==b->compression &&
        x->flags==y->flags && x->imageType==y->imageType && x->format==y->format &&
        x->extent.width==y->extent.width && x->extent.height==y->extent.height && x->extent.depth==y->extent.depth &&
        x->mipLevels==y->mipLevels && x->arrayLayers==y->arrayLayers && x->samples==y->samples &&
        x->tiling==y->tiling && x->usage==y->usage && x->sharingMode==y->sharingMode &&
        x->initialLayout==y->initialLayout && x->queueFamilyIndexCount==y->queueFamilyIndexCount &&
        (!x->queueFamilyIndexCount || !memcmp(a->queues,b->queues,(size_t)x->queueFamilyIndexCount*sizeof(*a->queues))) &&
        a->view_count==b->view_count && (!a->view_count || !memcmp(a->views,b->views,(size_t)a->view_count*sizeof(*a->views)));
}
VkResult hybris_bc_alias_prepare(struct hybris_bc_image *image,const VkImageCreateInfo *info,
    const VkMemoryRequirements *requirements,uint32_t memory_type,VkBool32 dedicated)
{
    struct hybris_bc_alias *r=calloc(1,sizeof(*r));
    if(!r)return VK_ERROR_OUT_OF_HOST_MEMORY;
    r->device=image->device;r->resolver=image->resolver;r->info=*info;
    r->info.pNext=NULL;r->info.pQueueFamilyIndices=NULL;
    r->requirements=*requirements;r->memory_type=memory_type;r->dedicated=dedicated;
    r->decoded_format=image->decoded_format;r->references=1;
    if(info->sharingMode==VK_SHARING_MODE_CONCURRENT && info->queueFamilyIndexCount) {
        if(info->queueFamilyIndexCount>SIZE_MAX/sizeof(*r->queues))goto oom;
        size_t bytes=(size_t)info->queueFamilyIndexCount*sizeof(*r->queues);
        r->queues=malloc(bytes);if(!r->queues)goto oom;
        memcpy(r->queues,info->pQueueFamilyIndices,bytes);
    } else r->info.queueFamilyIndexCount=0;
    for(const VkBaseInStructure *n=info->pNext;n;n=n->pNext) {
        if(n->sType==VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT) {
            r->compression_control=VK_TRUE;r->compression=((const VkImageCompressionControlEXT*)n)->flags;
        } else if(n->sType==VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *list=(const void*)n;
            r->view_count=list->viewFormatCount;
            if(r->view_count) {
                if(r->view_count>SIZE_MAX/sizeof(*r->views))goto oom;
                size_t bytes=(size_t)r->view_count*sizeof(*r->views);
                r->views=malloc(bytes);if(!r->views)goto oom;
                memcpy(r->views,list->pViewFormats,bytes);
            }
        }
    }
    image->alias=r;return VK_SUCCESS;
oom:
    discard(r);return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void unpublish(struct hybris_bc_alias *r)
{
    if(!r->backing)return;
    struct hybris_bc_alias **slot=&index[bucket(r->device,r->backing,r->offset)];
    while(*slot && *slot!=r)slot=&(*slot)->next;
    if(*slot)*slot=r->next;
    slot=&memory_index[bucket(r->device,r->backing,0)];
    while(*slot && *slot!=r)slot=&(*slot)->memory_next;
    if(*slot)*slot=r->memory_next;
    r->backing=VK_NULL_HANDLE;r->next=r->memory_next=NULL;
}
void hybris_bc_alias_release(struct hybris_bc_image *image)
{
    struct hybris_bc_alias *r=image->alias;
    if(!r)return;
    image->alias=NULL;image->decoded_memory=VK_NULL_HANDLE;
    pthread_mutex_lock(&guard);
    int last=--r->references==0;
    if(last)unpublish(r);
    pthread_mutex_unlock(&guard);
    if(last) {
        if(r->decoded)((PFN_vkFreeMemory)r->resolver(r->device,"vkFreeMemory"))(r->device,r->decoded,NULL);
        discard(r);
    }
}
void hybris_bc_alias_forget(VkDevice device,VkDeviceMemory memory)
{
    /* FreeMemory can precede image destruction. Invalidate the lookup key so
     * a recycled public memory handle cannot inherit the old allocation. */
    pthread_mutex_lock(&guard);
    struct hybris_bc_alias *r=memory_index[bucket(device,memory,0)];
    while(r) {
        struct hybris_bc_alias *next=r->memory_next;
        if(r->device==device && r->backing==memory)unpublish(r);
        r=next;
    }
    pthread_mutex_unlock(&guard);
}
VkResult hybris_bc_alias_bind(struct hybris_bc_image *image,VkDeviceMemory memory,VkDeviceSize offset)
{
    struct hybris_bc_alias *r=image->alias;
    if(!r)return VK_SUCCESS;
    PFN_vkBindImageMemory bind=(void*)r->resolver(r->device,"vkBindImageMemory");
    pthread_mutex_lock(&guard);
    size_t slot=bucket(r->device,memory,offset);
    for(struct hybris_bc_alias *other=index[slot];other;other=other->next)
        if(other->device==r->device && other->backing==memory && other->offset==offset && identical(r,other)) {
            if(r->dedicated || other->dedicated) {
                pthread_mutex_unlock(&guard);
                fprintf(stderr,"HYBRIS_BC_ALIAS native dedicated allocation prevents sharing decoded images\n");
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            if(other->references==SIZE_MAX) {pthread_mutex_unlock(&guard);return VK_ERROR_OUT_OF_HOST_MEMORY;}
            VkResult result=bind(r->device,image->image,other->decoded,0);
            if(result==VK_SUCCESS) {
                ++other->references;image->alias=other;image->decoded_memory=other->decoded;discard(r);
            }
            pthread_mutex_unlock(&guard);return result;
        }
    VkMemoryDedicatedAllocateInfo owner={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=image->image};
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext=r->dedicated?&owner:NULL,.allocationSize=r->requirements.size,.memoryTypeIndex=r->memory_type};
    VkResult result=((PFN_vkAllocateMemory)r->resolver(r->device,"vkAllocateMemory"))(
        r->device,&allocation,NULL,&r->decoded);
    if(result==VK_SUCCESS)result=bind(r->device,image->image,r->decoded,0);
    if(result==VK_SUCCESS) {
        r->backing=memory;r->offset=offset;r->next=index[slot];index[slot]=r;image->decoded_memory=r->decoded;
        size_t memory_slot=bucket(r->device,memory,0);
        r->memory_next=memory_index[memory_slot];memory_index[memory_slot]=r;
    } else if(r->decoded) {
        ((PFN_vkFreeMemory)r->resolver(r->device,"vkFreeMemory"))(r->device,r->decoded,NULL);r->decoded=VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&guard);return result;
}
