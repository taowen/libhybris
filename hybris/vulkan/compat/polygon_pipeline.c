/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Pipeline/index-stream approach adapted from MaliBridge 95a1e600,
 * server/server_emu_polygon.c. Modified to reuse the layer's lifetime journal.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "polygon_pipeline.h"
#include "../layer/layer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
static pthread_mutex_t pipeline_guard=PTHREAD_MUTEX_INITIALIZER;
static struct hybris_polygon_pipeline *pipelines;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int policy=1;
static void configure(void){const char *v=getauxval(AT_SECURE)?NULL:getenv("HYBRIS_POLYGON_MODE");if(v&&!strcmp(v,"0"))policy=0;else if(v&&!strcmp(v,"force"))policy=2;}
int hybris_polygon_emulated(VkPhysicalDevice physical)
{
    pthread_once(&once,configure);if(!policy)return 0;
    struct hybris_layer_physical owner;if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    VkPhysicalDeviceFeatures native;
    ((PFN_vkGetPhysicalDeviceFeatures)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures"))(physical,&native);
    return policy==2 || !native.fillModeNonSolid;
}
void hybris_polygon_note(const char *reason)
{
    static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
    static const char *reported[32];static unsigned count;
    pthread_mutex_lock(&guard);for(unsigned i=0;i<count;++i)if(!strcmp(reported[i],reason)){pthread_mutex_unlock(&guard);return;}
    if(count==32){pthread_mutex_unlock(&guard);return;}reported[count++]=reason;pthread_mutex_unlock(&guard);
    fprintf(stderr,"HYBRIS_POLYGON %s\n",reason);
}
void hybris_polygon_pipeline_destroy(struct hybris_polygon_pipeline *p)
{
    if(!p)return;const VkAllocationCallbacks *a=p->custom_allocator?&p->allocator:NULL;
    PFN_vkDestroyPipeline destroy=(void*)p->resolver(p->device,"vkDestroyPipeline");
    if(p->variant)destroy(p->device,p->variant,a);if(p->filled)destroy(p->device,p->filled,a);
    hybris_scaled_free(a,p);
}
void hybris_polygon_pipeline_publish(struct hybris_polygon_pipeline *p)
{
    pthread_mutex_lock(&pipeline_guard);
    p->references=1;p->next=pipelines;pipelines=p;
    pthread_mutex_unlock(&pipeline_guard);
}
struct hybris_polygon_pipeline *hybris_polygon_pipeline_acquire(VkDevice device,VkPipeline handle)
{
    struct hybris_polygon_pipeline *p=NULL;
    pthread_mutex_lock(&pipeline_guard);
    for(struct hybris_polygon_pipeline *it=pipelines;it;it=it->next)
        if(it->device==device && it->filled==handle) {
            if(it->references!=UINT32_MAX) {++it->references;p=it;}
            break;
        }
    pthread_mutex_unlock(&pipeline_guard);return p;
}
void hybris_polygon_pipeline_release(void *object)
{
    struct hybris_polygon_pipeline *p=object;
    if(!p) return;
    pthread_mutex_lock(&pipeline_guard);
    unsigned retire=!--p->references;
    pthread_mutex_unlock(&pipeline_guard);
    if(retire) hybris_polygon_pipeline_destroy(p);
}
int hybris_polygon_pipeline_remove(VkDevice device,VkPipeline handle)
{
    struct hybris_polygon_pipeline *p=NULL;
    pthread_mutex_lock(&pipeline_guard);
    for(struct hybris_polygon_pipeline **link=&pipelines;*link;link=&(*link)->next)
        if((*link)->device==device && (*link)->filled==handle) {
            p=*link;*link=p->next;p->next=NULL;break;
        }
    pthread_mutex_unlock(&pipeline_guard);
    if(p) hybris_polygon_pipeline_release(p);
    return p!=NULL;
}
void hybris_polygon_pipelines_release_device(VkDevice device)
{
    struct hybris_polygon_pipeline *removed=NULL;
    pthread_mutex_lock(&pipeline_guard);
    for(struct hybris_polygon_pipeline **link=&pipelines;*link;) {
        struct hybris_polygon_pipeline *p=*link;
        if(p->device!=device) {link=&p->next;continue;}
        *link=p->next;p->next=removed;removed=p;
    }
    pthread_mutex_unlock(&pipeline_guard);
    while(removed) {
        struct hybris_polygon_pipeline *next=removed->next;removed->next=NULL;
        hybris_polygon_pipeline_release(removed);removed=next;
    }
}


static int dynamic(const VkGraphicsPipelineCreateInfo *g,VkDynamicState state)
{for(uint32_t i=0;g->pDynamicState && i<g->pDynamicState->dynamicStateCount;++i)if(g->pDynamicState->pDynamicStates[i]==state)return 1;return 0;}
VkResult hybris_polygon_pipeline_create(VkDevice device,PFN_vkGetDeviceProcAddr resolver,
    const VkGraphicsPipelineCreateInfo *g,VkPolygonMode mode,const VkAllocationCallbacks *a,
    struct hybris_polygon_pipeline **out)
{
    *out=NULL;
    if(!g->pInputAssemblyState || !g->pRasterizationState || g->pRasterizationState->rasterizerDiscardEnable ||
       (mode!=VK_POLYGON_MODE_LINE && mode!=VK_POLYGON_MODE_POINT) ||
       (g->flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || dynamic(g,VK_DYNAMIC_STATE_POLYGON_MODE_EXT))return VK_ERROR_FEATURE_NOT_PRESENT;
    for(const VkBaseInStructure *n=g->pNext;n;n=n->pNext)if(n->sType==VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t i=0;i<g->stageCount;++i)if(g->pStages[i].stage!=VK_SHADER_STAGE_VERTEX_BIT && g->pStages[i].stage!=VK_SHADER_STAGE_FRAGMENT_BIT)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkPrimitiveTopology topology=g->pInputAssemblyState->topology;
    if(topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)return VK_ERROR_FEATURE_NOT_PRESENT;
    struct hybris_polygon_pipeline *p=hybris_scaled_alloc(a,sizeof(*p),VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    *p=(struct hybris_polygon_pipeline){.device=device,.resolver=resolver,.mode=mode,.topology=topology,
        .restart=g->pInputAssemblyState->primitiveRestartEnable,.custom_allocator=a!=NULL,
        .dynamic_topology=dynamic(g,VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY),.dynamic_restart=dynamic(g,VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)};
    if(a)p->allocator=*a;
    VkGraphicsPipelineCreateInfo variant=*g;
    VkPipelineInputAssemblyStateCreateInfo assembly=*g->pInputAssemblyState;
    assembly.topology=mode==VK_POLYGON_MODE_LINE?VK_PRIMITIVE_TOPOLOGY_LINE_LIST:VK_PRIMITIVE_TOPOLOGY_POINT_LIST;assembly.primitiveRestartEnable=VK_FALSE;
    VkPipelineRasterizationStateCreateInfo raster=*g->pRasterizationState;
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.depthBiasEnable=VK_FALSE;
    variant.pInputAssemblyState=&assembly;variant.pRasterizationState=&raster;
    variant.flags&=~(VK_PIPELINE_CREATE_DERIVATIVE_BIT|VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT|VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    variant.basePipelineHandle=VK_NULL_HANDLE;variant.basePipelineIndex=-1;
    VkResult result=((PFN_vkCreateGraphicsPipelines)resolver(device,"vkCreateGraphicsPipelines"))(device,VK_NULL_HANDLE,1,&variant,a,&p->variant);
    if(result!=VK_SUCCESS){hybris_polygon_pipeline_destroy(p);return result;}
    if(g->pRasterizationState->cullMode || g->pRasterizationState->depthBiasEnable)hybris_polygon_note("line/point variants do not preserve polygon culling or depth bias");
    hybris_polygon_note("variant uses original vertex shader, vertex input and application layout");*out=p;return VK_SUCCESS;
}
