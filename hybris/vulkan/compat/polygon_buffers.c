/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pattern caching, command scratch suballocation and descriptor pools adapted
 * from MaliBridge 95a1e600 server_emu_polygon.c and server_emu_cmdstate.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors.
 */
#define VK_NO_PROTOTYPES
#include "polygon_buffers.h"
#include <string.h>
#define PROC(n) PFN_vk##n n=(void*)device->resolver(device->handle,"vk" #n)
struct hybris_polygon_pattern {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t triangles;
    struct hybris_polygon_pattern *next;
};
struct polygon_chunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size, used;
    struct polygon_chunk *next;
};
struct polygon_pool {
    VkDescriptorPool handle;
    uint32_t used;
    struct polygon_pool *next;
};
struct hybris_polygon_storage {
    struct hybris_bc_device *device;
    VkAllocationCallbacks allocator;
    int custom;
    struct polygon_chunk *chunks;
    struct polygon_pool *pools;
};
static void release_buffer(struct hybris_bc_device *device, VkBuffer b, VkDeviceMemory m,
    const VkAllocationCallbacks *a)
{
    PROC(DestroyBuffer); PROC(FreeMemory);
    if (b) DestroyBuffer(device->handle, b, a);
    if (m) FreeMemory(device->handle, m, a);
}
static VkResult make_buffer(struct hybris_bc_device *device, VkDeviceSize size,
    int host, const VkAllocationCallbacks *a, VkBuffer *buffer, VkDeviceMemory *memory)
{
    *buffer = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE;
    PROC(CreateBuffer); PROC(GetBufferMemoryRequirements); PROC(AllocateMemory); PROC(BindBufferMemory);
    VkBufferCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=size,
        .usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkResult result = CreateBuffer(device->handle, &ci, a, buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements req;
    GetBufferMemoryRequirements(device->handle, *buffer, &req);
    uint32_t type = device->memory.memoryTypeCount;
    for (uint32_t i = 0; i < device->memory.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = device->memory.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || (host && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) continue;
        if (type == device->memory.memoryTypeCount) type = i;
        if (host ? (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    }
    if (type == device->memory.memoryTypeCount) result = VK_ERROR_FEATURE_NOT_PRESENT;
    else {
        VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=req.size, .memoryTypeIndex=type};
        result = AllocateMemory(device->handle, &ai, a, memory);
        if (result == VK_SUCCESS) result = BindBufferMemory(device->handle, *buffer, *memory, 0);
    }
    if (result != VK_SUCCESS) {
        release_buffer(device, *buffer, *memory, a);
        *buffer = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE;
    }
    return result;
}
static VkResult pattern_for(struct hybris_bc_device *device, uint32_t topology,
    uint32_t triangles, VkBuffer *out)
{
    pthread_mutex_lock(&device->guard);
    struct hybris_polygon_pattern *p = device->patterns[topology];
    VkResult result = VK_SUCCESS;
    if (!p || p->triangles < triangles) {
        uint64_t count = p ? (uint64_t)p->triangles * 2 : 1024;
        if (count < triangles) count = triangles;
        if (count > UINT32_MAX / 6) { result = VK_ERROR_FEATURE_NOT_PRESENT; goto done; }
        struct hybris_polygon_pattern *fresh = hybris_bc_alloc(device, sizeof(*fresh));
        if (!fresh) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        result = make_buffer(device, count * 24, 1, NULL, &fresh->buffer, &fresh->memory);
        if (result != VK_SUCCESS) { hybris_bc_free(device, fresh); goto done; }
        PROC(MapMemory); PROC(UnmapMemory); PROC(FlushMappedMemoryRanges);
        uint32_t *map;
        result = MapMemory(device->handle, fresh->memory, 0, VK_WHOLE_SIZE, 0, (void**)&map);
        if (result == VK_SUCCESS) {
            for (uint32_t t = 0; t < count; ++t) {
                uint32_t x = topology == 0 ? t*3 : topology == 1 ? t : 0;
                uint32_t y = topology == 0 ? t*3+1 : t+1, z = topology == 0 ? t*3+2 : t+2;
                uint32_t *o = map + (size_t)t * 6;
                o[0]=x; o[1]=y; o[2]=y; o[3]=z; o[4]=z; o[5]=x;
            }
            VkMappedMemoryRange range = {.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory=fresh->memory, .size=VK_WHOLE_SIZE};
            result = FlushMappedMemoryRanges(device->handle, 1, &range);
            UnmapMemory(device->handle, fresh->memory);
        }
        if (result != VK_SUCCESS) {
            release_buffer(device, fresh->buffer, fresh->memory, NULL);
            hybris_bc_free(device, fresh); goto done;
        }
        fresh->triangles = (uint32_t)count;
        /* Old recordings may still reference the preceding pattern. */
        if (p) { p->next=device->retired_patterns; device->retired_patterns=p; }
        device->patterns[topology] = p = fresh;
    }
    *out = p->buffer;
done:
    pthread_mutex_unlock(&device->guard);
    return result;
}
void hybris_polygon_patterns_destroy(struct hybris_bc_device *device)
{
    for (unsigned i=0; i<3; ++i) if (device->patterns[i]) {
        device->patterns[i]->next=device->retired_patterns;
        device->retired_patterns=device->patterns[i]; device->patterns[i]=NULL;
    }
    while (device->retired_patterns) {
        struct hybris_polygon_pattern *p=device->retired_patterns;
        device->retired_patterns=p->next;
        release_buffer(device,p->buffer,p->memory,NULL);hybris_bc_free(device,p);
    }
}
void hybris_polygon_storage_reset(struct hybris_polygon_storage *s)
{
    if (!s) return;
    struct hybris_bc_device *device=s->device;
    const VkAllocationCallbacks *a=s->custom?&s->allocator:NULL;
    PROC(ResetDescriptorPool);PROC(DestroyDescriptorPool);
    for (struct polygon_chunk *c=s->chunks;c;c=c->next)c->used=0;
    for (struct polygon_pool **p=&s->pools;*p;) {
        struct polygon_pool *pool=*p;
        if (ResetDescriptorPool(device->handle,pool->handle,0)!=VK_SUCCESS) {
            *p=pool->next;DestroyDescriptorPool(device->handle,pool->handle,a);hybris_scaled_free(a,pool);
        } else {pool->used=0;p=&pool->next;}
    }
}
void hybris_polygon_storage_destroy(struct hybris_polygon_storage *s)
{
    if (!s) return;
    struct hybris_bc_device *device=s->device;
    const VkAllocationCallbacks *a=s->custom?&s->allocator:NULL;
    PROC(DestroyDescriptorPool);
    while (s->pools) {struct polygon_pool *p=s->pools;s->pools=p->next;DestroyDescriptorPool(device->handle,p->handle,a);hybris_scaled_free(a,p);}
    while (s->chunks) {struct polygon_chunk *c=s->chunks;s->chunks=c->next;release_buffer(device,c->buffer,c->memory,a);hybris_scaled_free(a,c);}
    hybris_scaled_free(a,s);
}
static VkResult reserve(struct hybris_polygon_storage *s, VkDeviceSize size,
    VkBuffer *out, VkDeviceSize *offset, VkDescriptorSet *set)
{
    struct hybris_bc_device *device=s->device;
    const VkAllocationCallbacks *a=s->custom?&s->allocator:NULL;
    VkDeviceSize alignment=device->properties.limits.minStorageBufferOffsetAlignment;
    if (alignment<4)alignment=4;
    struct polygon_chunk *c=s->chunks;
    VkDeviceSize start=0;
    for (;c;c=c->next) {
        start=(c->used+alignment-1)/alignment*alignment;
        if (start<=c->size && size<=c->size-start)break;
    }
    VkResult result;
    if (!c) {
        c=hybris_scaled_alloc(a,sizeof(*c),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!c)return VK_ERROR_OUT_OF_HOST_MEMORY;
        *c=(struct polygon_chunk){.size=(size+(UINT64_C(1)<<20)-1)&~((UINT64_C(1)<<20)-1)};
        result=make_buffer(device,c->size,0,a,&c->buffer,&c->memory);
        if(result!=VK_SUCCESS){hybris_scaled_free(a,c);return result;}
        c->next=s->chunks;s->chunks=c;start=0;
    }
    struct polygon_pool *p=s->pools;
    while(p && p->used==64)p=p->next;
    PROC(CreateDescriptorPool);PROC(AllocateDescriptorSets);
    if(!p) {
        p=hybris_scaled_alloc(a,sizeof(*p),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
        *p=(struct polygon_pool){.next=s->pools};
        VkDescriptorPoolSize amount={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,128};
        VkDescriptorPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=64,.poolSizeCount=1,.pPoolSizes=&amount};
        result=CreateDescriptorPool(device->handle,&ci,a,&p->handle);
        if(result!=VK_SUCCESS){hybris_scaled_free(a,p);return result;}
        s->pools=p;
    }
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=p->handle,.descriptorSetCount=1,.pSetLayouts=&device->polygon.descriptors};
    result=AllocateDescriptorSets(device->handle,&ai,set);
    if(result!=VK_SUCCESS)return result;
    ++p->used;c->used=start+size;*out=c->buffer;*offset=start;
    return VK_SUCCESS;
}
void hybris_polygon_edges_destroy(void *object)
{
    struct hybris_polygon_edges *e=object;if(!e)return;
    const VkAllocationCallbacks *a=e->custom?&e->allocator:NULL;
    hybris_polygon_pipeline_release(e->pipeline);hybris_scaled_free(a,e);
}
VkResult hybris_polygon_edges_create(struct hybris_bc_device *device,struct hybris_polygon_pipeline *pipeline,
    struct hybris_polygon_storage **storage, uint32_t count,uint32_t topology,
    const VkDescriptorBufferInfo *source,uint32_t byte_offset,uint32_t index_size,
    VkBool32 restart,VkCommandBuffer prepass,const VkAllocationCallbacks *a,
    struct hybris_polygon_edges **out)
{
    *out=NULL;uint32_t tris=topology==0?count/3:count>=3?count-2:0;
    if(topology>2 || !tris || tris>UINT32_MAX/6)return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t size=(uint64_t)tris*24;
    if(size>device->properties.limits.maxStorageBufferRange)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct hybris_polygon_edges *e=hybris_scaled_alloc(a,sizeof(*e),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if(!e)return VK_ERROR_OUT_OF_HOST_MEMORY;
    *e=(struct hybris_polygon_edges){.device=device->handle,.resolver=device->resolver,.custom=a!=NULL};if(a)e->allocator=*a;
    VkResult r;
    if(!source)r=pattern_for(device,topology,tris,&e->buffer);
    else {
        pthread_mutex_lock(&device->guard);
        if(!device->polygon.device)r=hybris_polygon_indices_create(device->handle,device->resolver,NULL,&device->polygon);
        else r=VK_SUCCESS;
        pthread_mutex_unlock(&device->guard);
        if(r!=VK_SUCCESS)goto fail;
        if(!*storage) {
            *storage=hybris_scaled_alloc(a,sizeof(**storage),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            if(!*storage){r=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
            **storage=(struct hybris_polygon_storage){.device=device,.custom=a!=NULL};if(a)(*storage)->allocator=*a;
        }
        VkDescriptorSet set;
        r=reserve(*storage,size,&e->buffer,&e->offset,&set);if(r!=VK_SUCCESS)goto fail;
        PROC(UpdateDescriptorSets);
        VkDescriptorBufferInfo buffers[2]={*source,{e->buffer,e->offset,size}};
        VkWriteDescriptorSet writes[2];
        for(uint32_t i=0;i<2;++i)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
        UpdateDescriptorSets(device->handle,2,writes,0,NULL);
        uint64_t groups=((uint64_t)tris+63)/64,gx=groups,gy=1;
        if(gx>device->properties.limits.maxComputeWorkGroupCount[0]){gx=device->properties.limits.maxComputeWorkGroupCount[0];gy=(groups+gx-1)/gx;}
        if(!gx || gx>UINT32_MAX/64 || gy>device->properties.limits.maxComputeWorkGroupCount[1]){r=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
        struct hybris_polygon_push push={byte_offset,index_size,tris,topology,restart && topology==1,(uint32_t)gx*64};
        PROC(CmdPipelineBarrier);PROC(CmdBindPipeline);PROC(CmdBindDescriptorSets);PROC(CmdPushConstants);PROC(CmdDispatch);
        VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
        CmdPipelineBarrier(prepass,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
        CmdBindPipeline(prepass,VK_PIPELINE_BIND_POINT_COMPUTE,device->polygon.pipeline);
        CmdBindDescriptorSets(prepass,VK_PIPELINE_BIND_POINT_COMPUTE,device->polygon.layout,0,1,&set,0,NULL);
        CmdPushConstants(prepass,device->polygon.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        CmdDispatch(prepass,(uint32_t)gx,(uint32_t)gy,1);
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_INDEX_READ_BIT;
        CmdPipelineBarrier(prepass,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&barrier,0,NULL,0,NULL);
    }
    if(r!=VK_SUCCESS)goto fail;
    e->pipeline=pipeline;*out=e;return VK_SUCCESS;
fail:hybris_polygon_edges_destroy(e);return r;
}
