/* SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from MaliBridge server/server_emu_layer.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Private input copies, existing resource metadata and layer dispatch replace
 * the server's mutable decoded call arena and per-connection tables.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "maintenance5.h"
#include "dynamic_rendering.h"
#include "../layer/layer.h"
#include "memory_visibility.h"
#include "bc_context.h"
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static const void *find(const void *chain,VkStructureType type)
{
    for(const VkBaseInStructure *n=chain;n;n=n->pNext)if(n->sType==type)return n;
    return NULL;
}
int hybris_maintenance5_emulated(VkPhysicalDevice physical)
{
    if(getauxval(AT_SECURE))return 0;
    const char *option=getenv("HYBRIS_MAINTENANCE5");
    if(option && !strcmp(option,"0"))return 0;
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return 0;
    VkPhysicalDeviceProperties p;
    ((PFN_vkGetPhysicalDeviceProperties)owner.resolver(owner.instance,"vkGetPhysicalDeviceProperties"))(physical,&p);
    if(p.apiVersion<VK_API_VERSION_1_1)return 0;
    if(!(option && !strcmp(option,"force")) &&
        (p.apiVersion>=VK_API_VERSION_1_4 || hybris_dynamic_native_extension(physical,VK_KHR_MAINTENANCE_5_EXTENSION_NAME)))return 0;
    return p.apiVersion>=VK_API_VERSION_1_3 || hybris_dynamic_emulated(physical) ||
        hybris_dynamic_native_extension(physical,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
}
int hybris_maintenance5_requested(const VkDeviceCreateInfo *info)
{
    const VkPhysicalDeviceMaintenance5Features *f=find(info->pNext,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES);
    const VkPhysicalDeviceVulkan14Features *v=find(info->pNext,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
    return (f && f->maintenance5) || (v && v->maintenance5);
}
void hybris_maintenance5_features(VkPhysicalDevice physical,VkPhysicalDeviceFeatures2 *features)
{
    if(!hybris_maintenance5_emulated(physical))return;
    for(VkBaseOutStructure *n=features->pNext;n;n=n->pNext) {
        if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES)
            ((VkPhysicalDeviceMaintenance5Features*)n)->maintenance5=VK_TRUE;
        if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES)
            ((VkPhysicalDeviceVulkan14Features*)n)->maintenance5=VK_TRUE;
    }
}
void hybris_maintenance5_properties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *properties)
{
    if(!hybris_maintenance5_emulated(physical))return;
    for(VkBaseOutStructure *n=properties->pNext;n;n=n->pNext) {
        if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES) {
            VkPhysicalDeviceMaintenance5Properties *p=(void*)n;
            p->earlyFragmentMultisampleCoverageAfterSampleCounting=VK_FALSE;
            p->earlyFragmentSampleMaskTestBeforeSampleCounting=VK_FALSE;
            p->depthStencilSwizzleOneSupport=VK_FALSE;p->polygonModePointSize=VK_FALSE;
            p->nonStrictSinglePixelWideLinesUseParallelogram=VK_FALSE;
            p->nonStrictWideLinesUseParallelogram=VK_FALSE;
        }
    }
}
VkResult hybris_maintenance5_buffer_usage(const struct hybris_layer_device *device,
    const void *chain,VkBufferUsageFlags *usage)
{
    if(!device->maintenance5)return VK_SUCCESS;
    const VkBufferUsageFlags2CreateInfo *f=find(chain,VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO);
    if(!f)return VK_SUCCESS;
    if(f->usage>UINT32_MAX)return VK_ERROR_FEATURE_NOT_PRESENT;
    *usage=(VkBufferUsageFlags)f->usage;
    return VK_SUCCESS;
}
static VkResult flags(const void *chain,VkPipelineCreateFlags *out)
{
    const VkPipelineCreateFlags2CreateInfo *f=find(chain,VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
    if(!f)return VK_SUCCESS;
    if(f->flags>UINT32_MAX)return VK_ERROR_FEATURE_NOT_PRESENT;
    *out=(VkPipelineCreateFlags)f->flags;return VK_SUCCESS;
}
static VkResult VKAPI_CALL graphics(VkDevice handle,VkPipelineCache cache,uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *out)
{
    struct hybris_layer_device device;
    if(!hybris_layer_lookup_device(handle,&device))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateGraphicsPipelines next=(void*)hybris_layer_legacy_proc(handle,"vkCreateGraphicsPipelines");
    if(!device.maintenance5 || !count)return next(handle,cache,count,infos,allocator,out);
    for(uint32_t i=0;i<count;++i)out[i]=VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo *copy=calloc(count,sizeof(*copy));
    if(!copy)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy,infos,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i)result=flags(copy[i].pNext,&copy[i].flags);
    if(result==VK_SUCCESS)result=next(handle,cache,count,copy,allocator,out);
    free(copy);return result;
}
static VkResult VKAPI_CALL compute(VkDevice handle,VkPipelineCache cache,uint32_t count,
    const VkComputePipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *out)
{
    struct hybris_layer_device device;
    if(!hybris_layer_lookup_device(handle,&device))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateComputePipelines next=(void*)hybris_layer_legacy_proc(handle,"vkCreateComputePipelines");
    if(!device.maintenance5 || !count)return next(handle,cache,count,infos,allocator,out);
    for(uint32_t i=0;i<count;++i)out[i]=VK_NULL_HANDLE;
    VkComputePipelineCreateInfo *copy=calloc(count,sizeof(*copy));
    if(!copy)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copy,infos,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i)result=flags(copy[i].pNext,&copy[i].flags);
    if(result==VK_SUCCESS)result=next(handle,cache,count,copy,allocator,out);
    free(copy);return result;
}
static void VKAPI_CALL bind_index(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,VkIndexType type)
{
    struct hybris_layer_device device;
    if(!hybris_layer_command_device(command,&device))return;
    /* Native binding has no bounded range: valid accesses retain their native
     * behavior; the extra bound is still retained by the outer command journal. */
    if(!device.maintenance5) {
        PFN_vkCmdBindIndexBuffer2 next=(void*)device.resolver(device.handle,"vkCmdBindIndexBuffer2KHR");
        if(!next)next=(void*)device.resolver(device.handle,"vkCmdBindIndexBuffer2");
        if(next)next(command,buffer,offset,size,type);
        return;
    }
    ((PFN_vkCmdBindIndexBuffer)hybris_layer_legacy_proc(device.handle,"vkCmdBindIndexBuffer"))(command,buffer,offset,type);
}
static void VKAPI_CALL granularity(VkDevice handle,const VkRenderingAreaInfo *info,VkExtent2D *out)
{
    struct hybris_layer_device device;if(!hybris_layer_lookup_device(handle,&device))return;
    if(!device.maintenance5) {
        PFN_vkGetRenderingAreaGranularity next=(void*)device.resolver(handle,"vkGetRenderingAreaGranularityKHR");
        if(!next)next=(void*)device.resolver(handle,"vkGetRenderingAreaGranularity");
        if(next)next(handle,info,out);return;
    }
    *out=(VkExtent2D){1,1};
}
static void VKAPI_CALL layout2(VkDevice handle,VkImage image,const VkImageSubresource2 *sub,VkSubresourceLayout2 *out)
{
    struct hybris_layer_device device;
    if(!hybris_layer_lookup_device(handle,&device))return;
    PFN_vkGetImageSubresourceLayout2 native=(void*)device.resolver(handle,"vkGetImageSubresourceLayout2KHR");
    if(!native)native=(void*)device.resolver(handle,"vkGetImageSubresourceLayout2");
    if(!native)native=(void*)device.resolver(handle,"vkGetImageSubresourceLayout2EXT");
    if(native){native(handle,image,sub,out);return;}
    ((PFN_vkGetImageSubresourceLayout)hybris_layer_legacy_proc(handle,"vkGetImageSubresourceLayout"))(handle,image,&sub->imageSubresource,&out->subresourceLayout);
}
static void VKAPI_CALL device_layout(VkDevice handle,const VkDeviceImageSubresourceInfo *info,VkSubresourceLayout2 *out)
{
    struct hybris_layer_device device;if(!hybris_layer_lookup_device(handle,&device))return;
    if(!device.maintenance5) {
        PFN_vkGetDeviceImageSubresourceLayout next=(void*)device.resolver(handle,"vkGetDeviceImageSubresourceLayoutKHR");
        if(!next)next=(void*)device.resolver(handle,"vkGetDeviceImageSubresourceLayout");
        if(next)next(handle,info,out);return;
    }
    memset(&out->subresourceLayout,0,sizeof(out->subresourceLayout));
    VkImage image=VK_NULL_HANDLE;
    PFN_vkCreateImage create=(void*)hybris_layer_device_inner_proc(handle,"vkCreateImage");
    if(create(handle,info->pCreateInfo,NULL,&image)!=VK_SUCCESS)return;
    layout2(handle,image,info->pSubresource,out);
    ((PFN_vkDestroyImage)hybris_layer_device_inner_proc(handle,"vkDestroyImage"))(handle,image,NULL);
}
static void vertices(VkCommandBuffer command,uint32_t first,uint32_t count,const VkBuffer *buffers,
    const VkDeviceSize *offsets,const VkDeviceSize *sizes,const VkDeviceSize *strides,const char *name)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdBindVertexBuffers2 next=(void*)hybris_layer_legacy_proc(device.handle,name);
    if(!next){hybris_layer_command_error(command,VK_ERROR_FEATURE_NOT_PRESENT);return;}
    if(!device.maintenance5 || !sizes){next(command,first,count,buffers,offsets,sizes,strides);return;}
    int needed=0;for(uint32_t i=0;i<count;++i)needed|=sizes[i]==VK_WHOLE_SIZE && buffers[i];
    if(!needed){next(command,first,count,buffers,offsets,sizes,strides);return;}
    VkDeviceSize *copy=calloc(count,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,sizes,count*sizeof(*copy));
    for(uint32_t i=0;i<count;++i)if(copy[i]==VK_WHOLE_SIZE && buffers[i]) {
        VkDeviceSize total;VkResult result=hybris_memory_visibility_buffer_size(&device,buffers[i],&total);
        if(result!=VK_SUCCESS || offsets[i]>=total){free(copy);hybris_layer_command_error(command,VK_ERROR_INITIALIZATION_FAILED);return;}
        copy[i]=total-offsets[i];
    }
    next(command,first,count,buffers,offsets,copy,strides);free(copy);
}
static void VKAPI_CALL vertices_core(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,const VkDeviceSize *o,const VkDeviceSize *s,const VkDeviceSize *t)
{vertices(c,f,n,b,o,s,t,"vkCmdBindVertexBuffers2");}
static void VKAPI_CALL vertices_ext(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,const VkDeviceSize *o,const VkDeviceSize *s,const VkDeviceSize *t)
{vertices(c,f,n,b,o,s,t,"vkCmdBindVertexBuffers2EXT");}
static VkResult layers(const struct hybris_layer_device *device,VkImage image,VkImageSubresourceLayers *sub)
{
    if(sub->layerCount!=VK_REMAINING_ARRAY_LAYERS)return VK_SUCCESS;
    struct hybris_bc_device *owner=hybris_bc_device_find(device->handle);
    struct hybris_bc_resource *resource=owner?hybris_bc_resource_find(owner,image):NULL;
    if(!resource || sub->baseArrayLayer>=resource->layers)return VK_ERROR_INITIALIZATION_FAILED;
    sub->layerCount=resource->layers-sub->baseArrayLayer;return VK_SUCCESS;
}
static void VKAPI_CALL wrap_CmdCopyImage(VkCommandBuffer command,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageCopy *regions)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdCopyImage next=(void*)hybris_layer_legacy_proc(device.handle,"vkCmdCopyImage");
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<count;++i)needed|=regions[i].srcSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS || regions[i].dstSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,src,sl,dst,dl,count,regions);return;}
    VkImageCopy *copy=calloc(count,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,regions,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i){result=layers(&device,src,&copy[i].srcSubresource);if(result==VK_SUCCESS)result=layers(&device,dst,&copy[i].dstSubresource);}
    if(result==VK_SUCCESS)next(command,src,sl,dst,dl,count,copy);
    else hybris_layer_command_error(command,result);
    free(copy);
}
static void copy_image2(VkCommandBuffer command,const VkCopyImageInfo2 *info,const char *name)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdCopyImage2 next=(void*)hybris_layer_legacy_proc(device.handle,name);
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<info->regionCount;++i)needed|=info->pRegions[i].srcSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS || info->pRegions[i].dstSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,info);return;}
    VkImageCopy2 *copy=calloc(info->regionCount,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,info->pRegions,info->regionCount*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<info->regionCount && result==VK_SUCCESS;++i){result=layers(&device,info->srcImage,&copy[i].srcSubresource);if(result==VK_SUCCESS)result=layers(&device,info->dstImage,&copy[i].dstSubresource);}
    VkCopyImageInfo2 translated=*info;translated.pRegions=copy;
    if(result==VK_SUCCESS)next(command,&translated);else hybris_layer_command_error(command,result);
    free(copy);
}
static void VKAPI_CALL wrap_CmdCopyImage2(VkCommandBuffer command,const VkCopyImageInfo2 *info)
{copy_image2(command,info,"vkCmdCopyImage2");}
static void VKAPI_CALL wrap_CmdCopyImage2KHR(VkCommandBuffer command,const VkCopyImageInfo2 *info)
{copy_image2(command,info,"vkCmdCopyImage2KHR");}
static void VKAPI_CALL wrap_CmdResolveImage(VkCommandBuffer command,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageResolve *regions)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdResolveImage next=(void*)hybris_layer_legacy_proc(device.handle,"vkCmdResolveImage");
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<count;++i)needed|=regions[i].srcSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS || regions[i].dstSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,src,sl,dst,dl,count,regions);return;}
    VkImageResolve *copy=calloc(count,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,regions,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i){result=layers(&device,src,&copy[i].srcSubresource);if(result==VK_SUCCESS)result=layers(&device,dst,&copy[i].dstSubresource);}
    if(result==VK_SUCCESS)next(command,src,sl,dst,dl,count,copy);
    else hybris_layer_command_error(command,result);
    free(copy);
}
static void resolve_image2(VkCommandBuffer command,const VkResolveImageInfo2 *info,const char *name)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdResolveImage2 next=(void*)hybris_layer_legacy_proc(device.handle,name);
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<info->regionCount;++i)needed|=info->pRegions[i].srcSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS || info->pRegions[i].dstSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,info);return;}
    VkImageResolve2 *copy=calloc(info->regionCount,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,info->pRegions,info->regionCount*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<info->regionCount && result==VK_SUCCESS;++i){result=layers(&device,info->srcImage,&copy[i].srcSubresource);if(result==VK_SUCCESS)result=layers(&device,info->dstImage,&copy[i].dstSubresource);}
    VkResolveImageInfo2 translated=*info;translated.pRegions=copy;
    if(result==VK_SUCCESS)next(command,&translated);else hybris_layer_command_error(command,result);
    free(copy);
}
static void VKAPI_CALL wrap_CmdResolveImage2(VkCommandBuffer command,const VkResolveImageInfo2 *info)
{resolve_image2(command,info,"vkCmdResolveImage2");}
static void VKAPI_CALL wrap_CmdResolveImage2KHR(VkCommandBuffer command,const VkResolveImageInfo2 *info)
{resolve_image2(command,info,"vkCmdResolveImage2KHR");}
static void VKAPI_CALL wrap_CmdBlitImage(VkCommandBuffer command,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageBlit *regions,VkFilter filter)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdBlitImage next=(void*)hybris_layer_legacy_proc(device.handle,"vkCmdBlitImage");
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<count;++i)needed|=regions[i].srcSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS || regions[i].dstSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,src,sl,dst,dl,count,regions,filter);return;}
    VkImageBlit *copy=calloc(count,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,regions,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i){result=layers(&device,src,&copy[i].srcSubresource);if(result==VK_SUCCESS)result=layers(&device,dst,&copy[i].dstSubresource);}
    if(result==VK_SUCCESS)next(command,src,sl,dst,dl,count,copy,filter);
    else hybris_layer_command_error(command,result);
    free(copy);
}
static void blit_image2(VkCommandBuffer command,const VkBlitImageInfo2 *info,const char *name)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdBlitImage2 next=(void*)hybris_layer_legacy_proc(device.handle,name);
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<info->regionCount;++i)needed|=info->pRegions[i].srcSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS || info->pRegions[i].dstSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,info);return;}
    VkImageBlit2 *copy=calloc(info->regionCount,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,info->pRegions,info->regionCount*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<info->regionCount && result==VK_SUCCESS;++i){result=layers(&device,info->srcImage,&copy[i].srcSubresource);if(result==VK_SUCCESS)result=layers(&device,info->dstImage,&copy[i].dstSubresource);}
    VkBlitImageInfo2 translated=*info;translated.pRegions=copy;
    if(result==VK_SUCCESS)next(command,&translated);else hybris_layer_command_error(command,result);
    free(copy);
}
static void VKAPI_CALL wrap_CmdBlitImage2(VkCommandBuffer command,const VkBlitImageInfo2 *info)
{blit_image2(command,info,"vkCmdBlitImage2");}
static void VKAPI_CALL wrap_CmdBlitImage2KHR(VkCommandBuffer command,const VkBlitImageInfo2 *info)
{blit_image2(command,info,"vkCmdBlitImage2KHR");}
static void VKAPI_CALL wrap_CmdCopyBufferToImage(VkCommandBuffer command,VkBuffer buffer,VkImage image,VkImageLayout layout,uint32_t count,const VkBufferImageCopy *regions)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdCopyBufferToImage next=(void*)hybris_layer_legacy_proc(device.handle,"vkCmdCopyBufferToImage");
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<count;++i)needed|=regions[i].imageSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,buffer,image,layout,count,regions);return;}
    VkBufferImageCopy *copy=calloc(count,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,regions,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i){result=layers(&device,image,&copy[i].imageSubresource);}
    if(result==VK_SUCCESS)next(command,buffer,image,layout,count,copy);
    else hybris_layer_command_error(command,result);
    free(copy);
}
static void copy_buffer_image2(VkCommandBuffer command,const VkCopyBufferToImageInfo2 *info,const char *name)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdCopyBufferToImage2 next=(void*)hybris_layer_legacy_proc(device.handle,name);
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<info->regionCount;++i)needed|=info->pRegions[i].imageSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,info);return;}
    VkBufferImageCopy2 *copy=calloc(info->regionCount,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,info->pRegions,info->regionCount*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<info->regionCount && result==VK_SUCCESS;++i){result=layers(&device,info->dstImage,&copy[i].imageSubresource);}
    VkCopyBufferToImageInfo2 translated=*info;translated.pRegions=copy;
    if(result==VK_SUCCESS)next(command,&translated);else hybris_layer_command_error(command,result);
    free(copy);
}
static void VKAPI_CALL wrap_CmdCopyBufferToImage2(VkCommandBuffer command,const VkCopyBufferToImageInfo2 *info)
{copy_buffer_image2(command,info,"vkCmdCopyBufferToImage2");}
static void VKAPI_CALL wrap_CmdCopyBufferToImage2KHR(VkCommandBuffer command,const VkCopyBufferToImageInfo2 *info)
{copy_buffer_image2(command,info,"vkCmdCopyBufferToImage2KHR");}
static void VKAPI_CALL wrap_CmdCopyImageToBuffer(VkCommandBuffer command,VkImage image,VkImageLayout layout,VkBuffer buffer,uint32_t count,const VkBufferImageCopy *regions)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdCopyImageToBuffer next=(void*)hybris_layer_legacy_proc(device.handle,"vkCmdCopyImageToBuffer");
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<count;++i)needed|=regions[i].imageSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,image,layout,buffer,count,regions);return;}
    VkBufferImageCopy *copy=calloc(count,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,regions,count*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count && result==VK_SUCCESS;++i){result=layers(&device,image,&copy[i].imageSubresource);}
    if(result==VK_SUCCESS)next(command,image,layout,buffer,count,copy);
    else hybris_layer_command_error(command,result);
    free(copy);
}
static void copy_image_buffer2(VkCommandBuffer command,const VkCopyImageToBufferInfo2 *info,const char *name)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(command,&device))return;
    PFN_vkCmdCopyImageToBuffer2 next=(void*)hybris_layer_legacy_proc(device.handle,name);
    int needed=0;if(device.maintenance5)for(uint32_t i=0;i<info->regionCount;++i)needed|=info->pRegions[i].imageSubresource.layerCount==VK_REMAINING_ARRAY_LAYERS;
    if(!needed){next(command,info);return;}
    VkBufferImageCopy2 *copy=calloc(info->regionCount,sizeof(*copy));
    if(!copy){hybris_layer_command_error(command,VK_ERROR_OUT_OF_HOST_MEMORY);return;}
    memcpy(copy,info->pRegions,info->regionCount*sizeof(*copy));VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<info->regionCount && result==VK_SUCCESS;++i){result=layers(&device,info->srcImage,&copy[i].imageSubresource);}
    VkCopyImageToBufferInfo2 translated=*info;translated.pRegions=copy;
    if(result==VK_SUCCESS)next(command,&translated);else hybris_layer_command_error(command,result);
    free(copy);
}
static void VKAPI_CALL wrap_CmdCopyImageToBuffer2(VkCommandBuffer command,const VkCopyImageToBufferInfo2 *info)
{copy_image_buffer2(command,info,"vkCmdCopyImageToBuffer2");}
static void VKAPI_CALL wrap_CmdCopyImageToBuffer2KHR(VkCommandBuffer command,const VkCopyImageToBufferInfo2 *info)
{copy_image_buffer2(command,info,"vkCmdCopyImageToBuffer2KHR");}
PFN_vkVoidFunction hybris_maintenance5_added_proc(const char *name)
{
    if(!strcmp(name,"vkCmdBindIndexBuffer2") || !strcmp(name,"vkCmdBindIndexBuffer2KHR"))return (PFN_vkVoidFunction)bind_index;
    if(!strcmp(name,"vkGetRenderingAreaGranularity") || !strcmp(name,"vkGetRenderingAreaGranularityKHR"))return (PFN_vkVoidFunction)granularity;
    if(!strcmp(name,"vkGetImageSubresourceLayout2") || !strcmp(name,"vkGetImageSubresourceLayout2KHR"))return (PFN_vkVoidFunction)layout2;
    if(!strcmp(name,"vkGetDeviceImageSubresourceLayout") || !strcmp(name,"vkGetDeviceImageSubresourceLayoutKHR"))return (PFN_vkVoidFunction)device_layout;
    return NULL;
}
PFN_vkVoidFunction hybris_maintenance5_dispatch(const char *name)
{
#define PROC(n,f) if(!strcmp(name,"vk" #n))return (PFN_vkVoidFunction)f
    PROC(CmdCopyImage,wrap_CmdCopyImage);
    PROC(CmdCopyImage2,wrap_CmdCopyImage2);
    PROC(CmdCopyImage2KHR,wrap_CmdCopyImage2KHR);
    PROC(CmdResolveImage,wrap_CmdResolveImage);
    PROC(CmdResolveImage2,wrap_CmdResolveImage2);
    PROC(CmdResolveImage2KHR,wrap_CmdResolveImage2KHR);
    PROC(CmdBlitImage,wrap_CmdBlitImage);
    PROC(CmdBlitImage2,wrap_CmdBlitImage2);
    PROC(CmdBlitImage2KHR,wrap_CmdBlitImage2KHR);
    PROC(CmdCopyBufferToImage,wrap_CmdCopyBufferToImage);
    PROC(CmdCopyBufferToImage2,wrap_CmdCopyBufferToImage2);
    PROC(CmdCopyBufferToImage2KHR,wrap_CmdCopyBufferToImage2KHR);
    PROC(CmdCopyImageToBuffer,wrap_CmdCopyImageToBuffer);
    PROC(CmdCopyImageToBuffer2,wrap_CmdCopyImageToBuffer2);
    PROC(CmdCopyImageToBuffer2KHR,wrap_CmdCopyImageToBuffer2KHR);
    PROC(CmdBindVertexBuffers2,vertices_core);PROC(CmdBindVertexBuffers2EXT,vertices_ext);
    PROC(CreateGraphicsPipelines,graphics);PROC(CreateComputePipelines,compute);
#undef PROC
    return hybris_maintenance5_added_proc(name);
}
PFN_vkVoidFunction hybris_maintenance5_proc(const struct hybris_layer_device *device,const char *name)
{return device->maintenance5?hybris_maintenance5_dispatch(name):NULL;}
