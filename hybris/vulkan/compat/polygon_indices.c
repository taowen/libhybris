/* SPDX-License-Identifier: GPL-3.0-or-later */
#define VK_NO_PROTOTYPES
#include "polygon_indices.h"
#include <string.h>
#include "shaders/polygon_indices.inc"
#define PROC(n) PFN_vk##n n=(PFN_vk##n)p->resolver(p->device,"vk" #n)
void hybris_polygon_indices_destroy(struct hybris_polygon_indices *p,const VkAllocationCallbacks *a)
{
    if(!p || !p->resolver) return;
    PROC(DestroyPipeline); PROC(DestroyPipelineLayout); PROC(DestroyDescriptorSetLayout);
    if(p->pipeline) DestroyPipeline(p->device,p->pipeline,a);
    if(p->layout) DestroyPipelineLayout(p->device,p->layout,a);
    if(p->descriptors) DestroyDescriptorSetLayout(p->device,p->descriptors,a);
    memset(p,0,sizeof(*p));
}
VkResult hybris_polygon_indices_create(VkDevice device,PFN_vkGetDeviceProcAddr resolver,
    const VkAllocationCallbacks *a,struct hybris_polygon_indices *p)
{
    if(!p || !device || !resolver) return VK_ERROR_INITIALIZATION_FAILED;
    memset(p,0,sizeof(*p));p->device=device;p->resolver=resolver;
    PROC(CreateDescriptorSetLayout); PROC(CreatePipelineLayout); PROC(CreateShaderModule);
    PROC(CreateComputePipelines); PROC(DestroyShaderModule);
    PROC(DestroyPipeline); PROC(DestroyPipelineLayout); PROC(DestroyDescriptorSetLayout);
    if(!CreateDescriptorSetLayout || !CreatePipelineLayout || !CreateShaderModule || !CreateComputePipelines ||
        !DestroyShaderModule || !DestroyPipeline || !DestroyPipelineLayout || !DestroyDescriptorSetLayout) {
        memset(p,0,sizeof(*p));return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkDescriptorSetLayoutBinding bindings[2];
    for(unsigned i=0;i<2;++i) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sets={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings};
    VkResult result=CreateDescriptorSetLayout(device,&sets,a,&p->descriptors);
    if(result) goto fail;
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(struct hybris_polygon_push)};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&p->descriptors,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    result=CreatePipelineLayout(device,&layout,a,&p->layout);if(result) goto fail;
    VkShaderModuleCreateInfo module={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(hybris_polygon_indices_spv),.pCode=hybris_polygon_indices_spv};
    VkShaderModule shader=VK_NULL_HANDLE;
    result=CreateShaderModule(device,&module,a,&shader);if(result) goto fail;
    VkComputePipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout=p->layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    result=CreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline,a,&p->pipeline);
    DestroyShaderModule(device,shader,a);
    if(!result) return VK_SUCCESS;
fail:
    hybris_polygon_indices_destroy(p,a);return result;
}
