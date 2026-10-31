/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Index-stream conversion adapted from MaliBridge 95a1e600 server_emu_polygon.c.
 * Uses the existing command journal; no vertex-shader compute/replay path.
 */
#define VK_NO_PROTOTYPES
#include "polygon_record.h"
#include "shader_dispatch.h"
#include "memory_visibility.h"
#include "../layer/layer.h"
#include <string.h>
struct hybris_polygon_link {VkBuffer written;VkCommandBuffer child;uint64_t epoch;struct hybris_polygon_link *next;};
static struct hybris_polygon_recording *active(VkCommandBuffer c,struct hybris_layer_device *d)
{if(!hybris_layer_command_device(c,d)||!hybris_shader_polygon_active(d->handle))return NULL;return hybris_layer_polygon_recording(c);}
void hybris_polygon_record_reset(struct hybris_polygon_recording *s)
{
    const VkAllocationCallbacks *a=s->custom?&s->allocator:NULL;
    if(s->pre_pool)((PFN_vkDestroyCommandPool)s->resolver(s->device,"vkDestroyCommandPool"))(s->device,s->pre_pool,a);
    while(s->links){struct hybris_polygon_link *n=s->links->next;hybris_scaled_free(a,s->links);s->links=n;}
    s->pre_pool=VK_NULL_HANDLE;s->pre_command=VK_NULL_HANDLE;s->pre_open=s->has_prepass=0;s->edges=NULL;s->graphics=VK_NULL_HANDLE;s->index=VK_NULL_HANDLE;
    s->topology_known=s->restart_known=s->unknown_writes=0;
}
void hybris_polygon_record_index(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset,VkDeviceSize size,VkIndexType type)
{struct hybris_layer_device d;struct hybris_polygon_recording *s=active(c,&d);if(s){s->index=b;s->index_offset=offset;s->index_size=size;s->index_type=type;}}
static int written(const struct hybris_polygon_recording *s,VkBuffer b)
{if(s->unknown_writes)return 1;for(const struct hybris_polygon_link *l=s->links;l;l=l->next)if(l->written==b)return 1;return 0;}
void hybris_polygon_record_written(VkCommandBuffer c,VkBuffer b)
{
    struct hybris_layer_device d;struct hybris_polygon_recording *s=active(c,&d);if(!s||written(s,b))return;
    struct hybris_polygon_link *l=hybris_scaled_alloc(s->custom?&s->allocator:NULL,sizeof(*l),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if(!l){s->unknown_writes=1;hybris_layer_command_error(c,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    *l=(struct hybris_polygon_link){.written=b,.next=s->links};s->links=l;
}
void hybris_polygon_record_execute(VkCommandBuffer c,uint32_t count,const VkCommandBuffer *children)
{
    struct hybris_layer_device d;struct hybris_polygon_recording *s=active(c,&d);if(!s)return;
    for(uint32_t i=0;i<count;++i) {
        struct hybris_polygon_recording *child=hybris_layer_polygon_recording(children[i]);if(!child)continue;
        for(const struct hybris_polygon_edges *e=child->edges;e;e=e->next)if(e->pre&&written(s,e->source)) {
            hybris_polygon_note("unsupported: primary writes secondary prepass input; command rejected");hybris_layer_command_error(c,VK_ERROR_FEATURE_NOT_PRESENT);
        }
        struct hybris_polygon_link *l=hybris_scaled_alloc(s->custom?&s->allocator:NULL,sizeof(*l),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if(!l){hybris_layer_command_error(c,VK_ERROR_OUT_OF_HOST_MEMORY);continue;}
        *l=(struct hybris_polygon_link){.child=children[i],.epoch=child->epoch,.next=s->links};s->links=l;
        if(child->unknown_writes)s->unknown_writes=1;
        for(const struct hybris_polygon_link *w=child->links;w;w=w->next)if(w->written)hybris_polygon_record_written(c,w->written);
    }
    s->graphics=VK_NULL_HANDLE;s->index=VK_NULL_HANDLE;s->topology_known=s->restart_known=0;
}
uint32_t hybris_polygon_record_prepasses(VkCommandBuffer c,VkCommandBuffer *out,uint32_t capacity)
{
    const struct hybris_polygon_recording *s=hybris_layer_polygon_recording(c);uint64_t count=0;if(!s)return 0;
    if(s->has_prepass){if(s->pre_open)return UINT32_MAX;if(out&&count<capacity)out[count]=s->pre_command;++count;}
    for(const struct hybris_polygon_link *l=s->links;l;l=l->next)if(l->child) {
        const struct hybris_polygon_recording *child=hybris_layer_polygon_recording(l->child);
        if(!child||child->epoch!=l->epoch)return UINT32_MAX;
        if(child->has_prepass){if(child->pre_open)return UINT32_MAX;if(out&&count<capacity)out[count]=child->pre_command;++count;}
    }
    return count>=UINT32_MAX?UINT32_MAX:(uint32_t)count;
}
static VkResult prepass(struct hybris_polygon_recording *s,VkCommandBuffer *out)
{
    if(s->pre_command){*out=s->pre_command;return s->pre_open?VK_SUCCESS:VK_ERROR_FEATURE_NOT_PRESENT;}
    const VkAllocationCallbacks *a=s->custom?&s->allocator:NULL;
    if(!s->pre_pool){
        struct hybris_layer_device device;struct hybris_layer_physical physical;
        if(!hybris_layer_lookup_device(s->device,&device)||!hybris_layer_lookup_physical(device.physical,&physical))return VK_ERROR_INITIALIZATION_FAILED;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties query=(void*)physical.resolver(physical.instance,"vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t count=0;query(device.physical,&count,NULL);
        if(s->family>=count)return VK_ERROR_FEATURE_NOT_PRESENT;
        VkQueueFamilyProperties *families=hybris_scaled_alloc(a,(size_t)count*sizeof(*families),VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if(!families)return VK_ERROR_OUT_OF_HOST_MEMORY;
        query(device.physical,&count,families);int compute=s->family<count && (families[s->family].queueFlags&VK_QUEUE_COMPUTE_BIT);
        hybris_scaled_free(a,families);if(!compute)return VK_ERROR_FEATURE_NOT_PRESENT;
        VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=s->family};
        VkResult r=((PFN_vkCreateCommandPool)s->resolver(s->device,"vkCreateCommandPool"))(s->device,&pool,a,&s->pre_pool);if(r)return r;}
    VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=s->pre_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkResult r=((PFN_vkAllocateCommandBuffers)s->resolver(s->device,"vkAllocateCommandBuffers"))(s->device,&alloc,out);if(r)return r;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    r=((PFN_vkBeginCommandBuffer)s->resolver(s->device,"vkBeginCommandBuffer"))(*out,&begin);
    if(r==VK_SUCCESS){s->pre_command=*out;s->pre_open=1;}return r;
}
VkResult hybris_polygon_record_end(VkCommandBuffer c)
{
    struct hybris_polygon_recording *s=hybris_layer_polygon_recording(c);
    if(!s || !s->pre_open)return VK_SUCCESS;
    VkResult result=((PFN_vkEndCommandBuffer)s->resolver(s->device,"vkEndCommandBuffer"))(s->pre_command);s->pre_open=0;return result;
}
static PFN_vkVoidFunction promoted(const struct hybris_layer_device *d,const char *core,const char *extension)
{PFN_vkVoidFunction p=d->resolver(d->handle,core);return p?p:d->resolver(d->handle,extension);}
static int draw_polygon(VkCommandBuffer c,const struct hybris_layer_device *d,VkBool32 indexed,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex_offset,uint32_t first_instance)
{
    struct hybris_polygon_recording *s=hybris_layer_polygon_recording(c);if(!s||!s->graphics)return 0;
    struct hybris_polygon_pipeline *p=hybris_polygon_pipeline_acquire(d->handle,s->graphics);if(!p)return 0;
    VkPrimitiveTopology topology=p->dynamic_topology?s->topology:p->topology;VkBool32 restart=p->dynamic_restart?s->restart:p->restart;
    if((p->dynamic_topology&&!s->topology_known)||(p->dynamic_restart&&!s->restart_known))goto unsupported;
    uint32_t shape=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?0:topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP?1:topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN?2:3;
    if(shape>2)goto unsupported;
    if(!count||!instances){hybris_polygon_pipeline_release(p);return 1;}
    PFN_vkCmdSetPrimitiveTopology set_topology=(void*)promoted(d,"vkCmdSetPrimitiveTopology","vkCmdSetPrimitiveTopologyEXT");
    PFN_vkCmdSetPrimitiveRestartEnable set_restart=(void*)promoted(d,"vkCmdSetPrimitiveRestartEnable","vkCmdSetPrimitiveRestartEnableEXT");
    if((p->dynamic_topology&&!set_topology)||(p->dynamic_restart&&!set_restart))goto unsupported;
    const VkAllocationCallbacks *a=s->custom?&s->allocator:NULL;
    if(p->mode==VK_POLYGON_MODE_POINT) {
        if(indexed&&restart)goto unsupported;
        VkResult r=hybris_layer_command_retain(c,p,hybris_polygon_pipeline_release);if(r){hybris_polygon_pipeline_release(p);hybris_layer_command_error(c,r);return 0;}
    } else {
        uint32_t tris=shape==0?count/3:count>=3?count-2:0;
        if(!tris){hybris_polygon_pipeline_release(p);return 1;}
        if(tris>UINT32_MAX/6 || (!indexed&&first>INT32_MAX))goto unsupported;
        if(s->index && s->index_size!=VK_WHOLE_SIZE && !d->recording->CmdBindIndexBuffer2 && !d->recording->CmdBindIndexBuffer2KHR)goto unsupported;
        struct hybris_bc_device *bc=hybris_bc_device_find(d->handle);if(!bc)goto unsupported;
        VkDescriptorBufferInfo source;uint32_t byte_offset=0,index_size=0;VkCommandBuffer pre=VK_NULL_HANDLE;
        if(indexed) {
            if(!s->index||written(s,s->index)||(s->usage&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)||(restart&&(shape!=1 || vertex_offset<0)))goto unsupported;
            index_size=s->index_type==VK_INDEX_TYPE_UINT32?4:s->index_type==VK_INDEX_TYPE_UINT16?2:s->index_type==VK_INDEX_TYPE_UINT8_EXT?1:0;
            if(!index_size || (s->index_size!=VK_WHOLE_SIZE && ((uint64_t)first+count)*index_size>s->index_size))goto unsupported;
            uint64_t first_byte=s->index_offset+(uint64_t)first*index_size,alignment=bc->properties.limits.minStorageBufferOffsetAlignment;
            if(!alignment||first_byte<s->index_offset)goto unsupported;
            uint64_t base=first_byte/alignment*alignment,range=(first_byte-base+(uint64_t)count*index_size+3)&~UINT64_C(3);
            if(range>bc->properties.limits.maxStorageBufferRange||first_byte-base>UINT32_MAX)goto unsupported;
            if(hybris_memory_visibility_storage_buffer(d,s->index,base,range,&source)!=VK_SUCCESS)goto unsupported;
            byte_offset=(uint32_t)(first_byte-base);VkResult result=prepass(s,&pre);if(result){if(result!=VK_ERROR_FEATURE_NOT_PRESENT)hybris_layer_command_error(c,result);goto unsupported;}
        }
        struct hybris_polygon_edges *edges=NULL;
        VkResult r=hybris_polygon_edges_create(bc,p,count,shape,indexed?&source:NULL,byte_offset,index_size,restart,pre,a,&edges);
        if(r!=VK_SUCCESS){if(r!=VK_ERROR_FEATURE_NOT_PRESENT)hybris_layer_command_error(c,r);goto unsupported;}
        r=hybris_layer_command_retain(c,edges,hybris_polygon_edges_destroy);if(r){hybris_polygon_edges_destroy(edges);hybris_layer_command_error(c,r);return 0;}
        if(pre)s->has_prepass=1;edges->pre=pre;edges->source=indexed?s->index:VK_NULL_HANDLE;edges->next=s->edges;s->edges=edges;
        ((PFN_vkCmdBindPipeline)d->resolver(d->handle,"vkCmdBindPipeline"))(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p->variant);
        if(p->dynamic_topology)set_topology(c,VK_PRIMITIVE_TOPOLOGY_LINE_LIST);if(p->dynamic_restart)set_restart(c,VK_FALSE);
        ((PFN_vkCmdBindIndexBuffer)d->resolver(d->handle,"vkCmdBindIndexBuffer"))(c,edges->buffer,0,VK_INDEX_TYPE_UINT32);
        d->recording->CmdDrawIndexed(c,tris*6,instances,0,indexed?vertex_offset:(int32_t)first,first_instance);
        if(s->index) {
            if(s->index_size!=VK_WHOLE_SIZE)(d->recording->CmdBindIndexBuffer2?d->recording->CmdBindIndexBuffer2:d->recording->CmdBindIndexBuffer2KHR)(c,s->index,s->index_offset,s->index_size,s->index_type);
            else d->recording->CmdBindIndexBuffer(c,s->index,s->index_offset,s->index_type);
        }
        goto restore;
    }
    ((PFN_vkCmdBindPipeline)d->resolver(d->handle,"vkCmdBindPipeline"))(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p->variant);
    if(p->dynamic_topology)set_topology(c,VK_PRIMITIVE_TOPOLOGY_POINT_LIST);if(p->dynamic_restart)set_restart(c,VK_FALSE);
    if(indexed)d->recording->CmdDrawIndexed(c,count,instances,first,vertex_offset,first_instance);else d->recording->CmdDraw(c,count,instances,first,first_instance);
restore:
    ((PFN_vkCmdBindPipeline)d->resolver(d->handle,"vkCmdBindPipeline"))(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p->filled);
    if(p->dynamic_topology)set_topology(c,topology);if(p->dynamic_restart)set_restart(c,restart);
    hybris_polygon_note(indexed?"indexed line/point path":"direct line/point path");return 1;
unsupported:hybris_polygon_note("unsupported draw uses full filled fallback (writes, restart, topology, bounds or simultaneous use)");hybris_polygon_pipeline_release(p);return 0;
}
static void VKAPI_CALL draw(VkCommandBuffer c,uint32_t n,uint32_t i,uint32_t first,uint32_t base)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;if(!hybris_shader_polygon_active(d.handle)||!draw_polygon(c,&d,0,n,i,first,0,base))d.recording->CmdDraw(c,n,i,first,base);}
static void VKAPI_CALL draw_indexed(VkCommandBuffer c,uint32_t n,uint32_t i,uint32_t first,int32_t offset,uint32_t base)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;if(!hybris_shader_polygon_active(d.handle)||!draw_polygon(c,&d,1,n,i,first,offset,base))d.recording->CmdDrawIndexed(c,n,i,first,offset,base);}
static void VKAPI_CALL bind(VkCommandBuffer c,VkPipelineBindPoint point,VkPipeline p)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;d.recording->CmdBindPipeline(c,point,p);if(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&hybris_shader_polygon_active(d.handle)){struct hybris_polygon_recording *s=hybris_layer_polygon_recording(c);if(s)s->graphics=p;}}
static void topology(VkCommandBuffer c,VkPrimitiveTopology t,int ext)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;((PFN_vkCmdSetPrimitiveTopology)d.resolver(d.handle,ext?"vkCmdSetPrimitiveTopologyEXT":"vkCmdSetPrimitiveTopology"))(c,t);struct hybris_polygon_recording *s=active(c,&d);if(s){s->topology=t;s->topology_known=1;}}
static void restart(VkCommandBuffer c,VkBool32 v,int ext)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;((PFN_vkCmdSetPrimitiveRestartEnable)d.resolver(d.handle,ext?"vkCmdSetPrimitiveRestartEnableEXT":"vkCmdSetPrimitiveRestartEnable"))(c,v);struct hybris_polygon_recording *s=active(c,&d);if(s){s->restart=v;s->restart_known=1;}}
static void VKAPI_CALL topology_core(VkCommandBuffer c,VkPrimitiveTopology t){topology(c,t,0);}static void VKAPI_CALL topology_ext(VkCommandBuffer c,VkPrimitiveTopology t){topology(c,t,1);}
static void VKAPI_CALL restart_core(VkCommandBuffer c,VkBool32 v){restart(c,v,0);}static void VKAPI_CALL restart_ext(VkCommandBuffer c,VkBool32 v){restart(c,v,1);}
static void VKAPI_CALL dispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;struct hybris_polygon_recording *s=active(c,&d);if(s)s->unknown_writes=1;((PFN_vkCmdDispatch)d.resolver(d.handle,"vkCmdDispatch"))(c,x,y,z);}
static void VKAPI_CALL dispatch_indirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o)
{struct hybris_layer_device d;if(!hybris_layer_command_device(c,&d))return;struct hybris_polygon_recording *s=active(c,&d);if(s)s->unknown_writes=1;((PFN_vkCmdDispatchIndirect)d.resolver(d.handle,"vkCmdDispatchIndirect"))(c,b,o);}
PFN_vkVoidFunction hybris_polygon_record_proc(const char *name)
{
#define ENTRY(n,f) if(!strcmp(name,"vk" #n))return (PFN_vkVoidFunction)f
    ENTRY(CmdDraw,draw);ENTRY(CmdDrawIndexed,draw_indexed);ENTRY(CmdBindPipeline,bind);
    ENTRY(CmdSetPrimitiveTopology,topology_core);ENTRY(CmdSetPrimitiveTopologyEXT,topology_ext);
    ENTRY(CmdSetPrimitiveRestartEnable,restart_core);ENTRY(CmdSetPrimitiveRestartEnableEXT,restart_ext);
    ENTRY(CmdDispatch,dispatch);ENTRY(CmdDispatchIndirect,dispatch_indirect);
#undef ENTRY
    return NULL;
}
