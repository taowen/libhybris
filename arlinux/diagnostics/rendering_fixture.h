/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "vulkan_context.h"
#include "viewport_vert.inc"
#include "viewport_frag.inc"
#include <array>
using namespace diagnostics;
namespace rendering_diagnostics {
constexpr uint32_t W=32,H=16;
struct Graphics {
    Context &c;ImageResources image;bool rendering;bool maintenance;VkCommandBuffer secondary=VK_NULL_HANDLE;unsigned runs=0;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer frame=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    Graphics(Context &context,bool use_rendering,bool use_maintenance=false):c(context),image(c),rendering(use_rendering),maintenance(use_maintenance) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;
        ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={W,H,1};ci.mipLevels=ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        CHECK(c.CreateImage(c.device,&ci,nullptr,&image.image));
        VkMemoryRequirements need;c.GetImageMemoryRequirements(c.device,image.image,&need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=need.size;alloc.memoryTypeIndex=c.memory_type(need.memoryTypeBits,0);
        CHECK(c.AllocateMemory(c.device,&alloc,nullptr,&image.memory));CHECK(c.BindImageMemory(c.device,image.image,image.memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        CHECK(c.CreateImageView(c.device,&vi,nullptr,&image.view));
        VkAttachmentDescription attachment{};attachment.format=ci.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=1;rp.pDependencies=&dependency;
        CHECK(c.CreateRenderPass(c.device,&rp,nullptr,&pass));
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass;fi.attachmentCount=1;fi.pAttachments=&image.view;fi.width=W;fi.height=H;fi.layers=1;
        CHECK(c.CreateFramebuffer(c.device,&fi,nullptr,&frame));
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};CHECK(c.CreatePipelineLayout(c.device,&li,nullptr,&layout));
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(viewport_vert_spv);sm.pCode=viewport_vert_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&vertex));
        sm.codeSize=sizeof(viewport_frag_spv);sm.pCode=viewport_frag_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&fragment));
    }
    ~Graphics(){if(!c.idle)return;if(secondary)c.FreeCommandBuffers(c.device,c.pool,1,&secondary);if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);if(vertex)c.DestroyShaderModule(c.device,vertex,nullptr);if(fragment)c.DestroyShaderModule(c.device,fragment,nullptr);if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);if(frame)c.DestroyFramebuffer(c.device,frame,nullptr);if(pass)c.DestroyRenderPass(c.device,pass,nullptr);}
    bool render(Buffer &output,bool dynamic,int resolver,bool use_secondary) {
        if(pipeline){c.DestroyPipeline(c.device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        VkViewport views[]={{0,0,W/2.0f,float(H),0,1},{W/2.0f,0,W/2.0f,float(H),0,1}};
        VkRect2D scissors[]={{{0,0},{W,H/2}},{{0,H/2},{W,H/2}}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=views;vp.pScissors=scissors;
        const auto before=vp;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(auto &s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&color;
        VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=states;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&samples;pi.pColorBlendState=&blend;pi.pDynamicState=dynamic?&dyn:nullptr;pi.layout=layout;pi.renderPass=rendering?VK_NULL_HANDLE:pass;
        VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;VkPipelineRenderingCreateInfo rendering_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering_info.colorAttachmentCount=1;rendering_info.pColorAttachmentFormats=&format;if(rendering)pi.pNext=&rendering_info;
        VkShaderModuleCreateInfo modules[2]{};
        if(maintenance) {
            for(auto &module:modules)module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            modules[0].codeSize=sizeof(viewport_vert_spv);modules[0].pCode=viewport_vert_spv;
            modules[1].codeSize=sizeof(viewport_frag_spv);modules[1].pCode=viewport_frag_spv;
            for(unsigned i=0;i<2;++i){stages[i].module=VK_NULL_HANDLE;stages[i].pNext=&modules[i];}
        }
        VkPipelineCreateFlags2CreateInfo flags2{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
        flags2.flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;flags2.pNext=pi.pNext;
        if(maintenance)pi.pNext=&flags2;
        const auto pipeline_input=pi;const auto rendering_input=rendering_info;const auto original_flags=flags2;
        const std::array<VkPipelineShaderStageCreateInfo,2> original_stages={stages[0],stages[1]};
        CHECK(c.CreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
        if(memcmp(&pipeline_input,&pi,sizeof(pi)) || memcmp(&rendering_input,&rendering_info,sizeof(rendering_info)))throw std::runtime_error("Caller pipeline create info changed");
        if(memcmp(&flags2,&original_flags,sizeof(flags2)) || memcmp(stages,original_stages.data(),sizeof(stages)))throw std::runtime_error("Caller flags or inline stages changed");
        if(memcmp(&before,&vp,sizeof(vp)))throw std::runtime_error("Application pipeline state mutated");
        if(image.command && (++runs%2)) {
            CHECK(((PFN_vkResetCommandPool)c.gdpa(c.device,"vkResetCommandPool"))(c.device,c.pool,0));
        } else {
            if(secondary){c.FreeCommandBuffers(c.device,c.pool,1,&secondary);secondary=VK_NULL_HANDLE;}
            if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}
        }
        if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=c.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        if(!image.command)CHECK(c.AllocateCommandBuffers(c.device,&ca,&image.command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        VkClearValue clear{};clear.color.float32[3]=1;
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=pass;rb.framebuffer=frame;rb.renderArea={{0,0},{W,H}};rb.clearValueCount=1;rb.pClearValues=&clear;
        auto rendering_proc=[&](const char *name){auto p=resolver?c.gipa(c.instance,name):c.gdpa(c.device,name);if(!p)throw std::runtime_error(std::string("Missing ")+name);return p;};
        if(rendering) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment.imageView=image.view;attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.clearValue=clear;
            VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea={{0,0},{W,H}};info.layerCount=1;info.colorAttachmentCount=1;info.pColorAttachments=&attachment;
            if(use_secondary)info.flags=VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
            const auto original_info=info;const auto original_attachment=attachment;
            ((PFN_vkCmdBeginRenderingKHR)rendering_proc("vkCmdBeginRenderingKHR"))(image.command,&info);
            if(memcmp(&info,&original_info,sizeof(info)) || memcmp(&attachment,&original_attachment,sizeof(attachment)))throw std::runtime_error("Caller rendering data changed");
        } else c.CmdBeginRenderPass(image.command,&rb,use_secondary?VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS:VK_SUBPASS_CONTENTS_INLINE);
        VkCommandBuffer draw=image.command;
        if(use_secondary) {
            ca.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            if(!secondary)CHECK(c.AllocateCommandBuffers(c.device,&ca,&secondary));
            VkCommandBufferInheritanceRenderingInfo formats{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
            formats.colorAttachmentCount=1;formats.pColorAttachmentFormats=&format;formats.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
            inheritance.renderPass=rendering?VK_NULL_HANDLE:pass;inheritance.pNext=rendering?&formats:nullptr;
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;bi.pInheritanceInfo=&inheritance;
            const auto original_inheritance=inheritance;const auto original_formats=formats;
            CHECK(c.BeginCommandBuffer(secondary,&bi));
            if(memcmp(&inheritance,&original_inheritance,sizeof(inheritance)) || memcmp(&formats,&original_formats,sizeof(formats)))throw std::runtime_error("Caller secondary inheritance changed");
            draw=secondary;
        }
        c.CmdBindPipeline(draw,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        auto proc=[&](const char *name){auto p=resolver?c.gipa(c.instance,name):c.gdpa(c.device,name);if(!p)throw std::runtime_error(std::string("Missing ")+name);return p;};
        if(dynamic) {
            ((PFN_vkCmdSetViewport)proc("vkCmdSetViewport"))(draw,0,1,views);
            ((PFN_vkCmdSetScissor)proc("vkCmdSetScissor"))(draw,0,1,scissors);
        }
        Buffer indices(c,3*sizeof(uint16_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,maintenance);
        if(maintenance) {
            const uint16_t data[]={0,1,2};memcpy(indices.mapped,data,sizeof(data));indices.visibility(true);
            ((PFN_vkCmdBindIndexBuffer2KHR)proc("vkCmdBindIndexBuffer2KHR"))(draw,indices.handle,0,sizeof(data),VK_INDEX_TYPE_UINT16);
            ((PFN_vkCmdDrawIndexed)proc("vkCmdDrawIndexed"))(draw,3,1,0,0,0);
        } else c.CmdDraw(draw,3,1,0,0);
        if(use_secondary) {
            CHECK(c.EndCommandBuffer(secondary));
            ((PFN_vkCmdExecuteCommands)proc("vkCmdExecuteCommands"))(image.command,1,&secondary);
        }
        if(rendering) {
            ((PFN_vkCmdEndRenderingKHR)rendering_proc("vkCmdEndRenderingKHR"))(image.command);
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        } else c.CmdEndRenderPass(image.command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,maintenance?VK_REMAINING_ARRAY_LAYERS:1};copy.imageExtent={W,H,1};const auto original_copy=copy;c.CmdCopyImageToBuffer(image.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        if(memcmp(&copy,&original_copy,sizeof(copy)))throw std::runtime_error("Caller copy region changed");
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(c.EndCommandBuffer(image.command));VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&image.command;CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);
        auto pixels=(const uint8_t*)output.mapped;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const auto *p=pixels+(y*W+x)*4;bool red=x<W/2 && y<H/2;if(p[0]!=(red?255:0)||p[1]||p[2]||p[3]!=255)return false;}
        return true;
    }
};
}
