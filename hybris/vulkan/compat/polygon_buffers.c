/* SPDX-License-Identifier: GPL-3.0-or-later
 * Index-stream conversion adapted from MaliBridge 95a1e600 server_emu_polygon.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors.
 */
#define VK_NO_PROTOTYPES
#include "polygon_buffers.h"
#include <string.h>
#define PROC(n) PFN_vk##n n=(void*)device->resolver(device->handle,"vk" #n)
void hybris_polygon_edges_destroy(void *object)
{
    struct hybris_polygon_edges *e=object;if(!e)return;
    const VkAllocationCallbacks *a=e->custom?&e->allocator:NULL;
    if(e->descriptors)((PFN_vkDestroyDescriptorPool)e->resolver(e->device,"vkDestroyDescriptorPool"))(e->device,e->descriptors,a);
    if(e->buffer)((PFN_vkDestroyBuffer)e->resolver(e->device,"vkDestroyBuffer"))(e->device,e->buffer,a);
    if(e->memory)((PFN_vkFreeMemory)e->resolver(e->device,"vkFreeMemory"))(e->device,e->memory,a);
    hybris_polygon_pipeline_release(e->pipeline);hybris_scaled_free(a,e);
}
VkResult hybris_polygon_edges_create(struct hybris_bc_device *device,struct hybris_polygon_pipeline *pipeline,
    uint32_t count,uint32_t topology,const VkDescriptorBufferInfo *source,uint32_t byte_offset,
    uint32_t index_size,VkBool32 restart,VkCommandBuffer prepass,const VkAllocationCallbacks *a,
    struct hybris_polygon_edges **out)
{
    *out=NULL;uint32_t tris=topology==0?count/3:count>=3?count-2:0;
    if(!tris || tris>UINT32_MAX/6)return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t size=(uint64_t)tris*24;
    if(size>device->properties.limits.maxStorageBufferRange)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct hybris_polygon_edges *e=hybris_scaled_alloc(a,sizeof(*e),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if(!e)return VK_ERROR_OUT_OF_HOST_MEMORY;
    *e=(struct hybris_polygon_edges){.device=device->handle,.resolver=device->resolver,.custom=a!=NULL};if(a)e->allocator=*a;
    PROC(CreateBuffer);PROC(GetBufferMemoryRequirements);PROC(AllocateMemory);PROC(BindBufferMemory);
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkResult r=CreateBuffer(device->handle,&buffer,a,&e->buffer);if(r)goto fail;
    VkMemoryRequirements req;GetBufferMemoryRequirements(device->handle,e->buffer,&req);
    uint32_t type=device->memory.memoryTypeCount;
    for(uint32_t i=0;i<device->memory.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (source || (device->memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))){type=i;break;}
    if(type==device->memory.memoryTypeCount){r=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;}
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    r=AllocateMemory(device->handle,&allocation,a,&e->memory);if(r)goto fail;
    r=BindBufferMemory(device->handle,e->buffer,e->memory,0);if(r)goto fail;
    if(!source) {
        PROC(MapMemory);PROC(FlushMappedMemoryRanges);PROC(UnmapMemory);uint32_t *indices=NULL;
        r=MapMemory(device->handle,e->memory,0,VK_WHOLE_SIZE,0,(void**)&indices);if(r)goto fail;
        for(uint32_t t=0;t<tris;++t){uint32_t x=topology==0?t*3:topology==1?t:0,y=topology==0?t*3+1:t+1,z=topology==0?t*3+2:t+2;
            indices[t*6]=x;indices[t*6+1]=y;indices[t*6+2]=y;indices[t*6+3]=z;indices[t*6+4]=z;indices[t*6+5]=x;}
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=e->memory,.size=VK_WHOLE_SIZE};
        r=FlushMappedMemoryRanges(device->handle,1,&range);UnmapMemory(device->handle,e->memory);if(r)goto fail;
    } else {
        pthread_mutex_lock(&device->guard);
        if(!device->polygon.device)r=hybris_polygon_indices_create(device->handle,device->resolver,NULL,&device->polygon);
        pthread_mutex_unlock(&device->guard);if(r)goto fail;
        PROC(CreateDescriptorPool);PROC(AllocateDescriptorSets);PROC(UpdateDescriptorSets);
        VkDescriptorPoolSize amount={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
        VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&amount};
        r=CreateDescriptorPool(device->handle,&pool,a,&e->descriptors);if(r)goto fail;
        VkDescriptorSet set;VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=e->descriptors,.descriptorSetCount=1,.pSetLayouts=&device->polygon.descriptors};
        r=AllocateDescriptorSets(device->handle,&alloc,&set);if(r)goto fail;
        VkDescriptorBufferInfo buffers[2]={*source,{e->buffer,0,size}};VkWriteDescriptorSet writes[2];
        for(uint32_t i=0;i<2;++i)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
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
    e->pipeline=pipeline;*out=e;return VK_SUCCESS;
fail:hybris_polygon_edges_destroy(e);return r;
}
