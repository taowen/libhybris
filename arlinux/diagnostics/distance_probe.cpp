/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "distance_clip_vert.inc"
#include "distance_cull_vert.inc"
#include "distance_both_vert.inc"
#include "distance_frag.inc"
#include "distance_read_frag.inc"
#include "distance_gs_vert.inc"
#include "distance_gs.inc"
#include "distance_point_geom.inc"
#include "viewport_frag.inc"
using namespace diagnostics;
namespace {
constexpr uint32_t W=32,H=16;
struct Graphics {
    Context &c;ImageResources image;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer frame=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE,geometry=VK_NULL_HANDLE;
    Graphics(Context &context,unsigned variant):c(context),image(c) {
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
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};const uint32_t *vertices[]={distance_clip_vert_spv,distance_cull_vert_spv,distance_both_vert_spv,distance_clip_vert_spv,distance_gs_vert_spv,distance_both_vert_spv};
        const size_t sizes[]={sizeof(distance_clip_vert_spv),sizeof(distance_cull_vert_spv),sizeof(distance_both_vert_spv),sizeof(distance_clip_vert_spv),sizeof(distance_gs_vert_spv),sizeof(distance_both_vert_spv)};
        sm.codeSize=sizes[variant];sm.pCode=vertices[variant];CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&vertex));
        sm.codeSize=variant==4?sizeof(viewport_frag_spv):variant==3?sizeof(distance_read_frag_spv):sizeof(distance_frag_spv);sm.pCode=variant==4?viewport_frag_spv:variant==3?distance_read_frag_spv:distance_frag_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&fragment));
        if(variant>=4){sm.codeSize=variant==4?sizeof(distance_gs_spv):sizeof(distance_point_geom_spv);
            sm.pCode=variant==4?distance_gs_spv:distance_point_geom_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&geometry));}
    }
    ~Graphics(){if(!c.idle)return;if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);if(geometry)c.DestroyShaderModule(c.device,geometry,nullptr);if(vertex)c.DestroyShaderModule(c.device,vertex,nullptr);if(fragment)c.DestroyShaderModule(c.device,fragment,nullptr);if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);if(frame)c.DestroyFramebuffer(c.device,frame,nullptr);if(pass)c.DestroyRenderPass(c.device,pass,nullptr);}
    bool render(Buffer &output,bool dynamic,unsigned variant) {
        if(pipeline){c.DestroyPipeline(c.device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        VkViewport view{0,0,float(W),float(H),0,1};
        VkRect2D scissor{{0,0},{W,H}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount=vp.scissorCount=1;vp.pViewports=&view;vp.pScissors=&scissor;
        VkPipelineShaderStageCreateInfo stages[3]{};
        for(auto &s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;stages[2].stage=VK_SHADER_STAGE_GEOMETRY_BIT;stages[2].module=geometry;
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&color;
        VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&format;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext=dynamic?&rendering:nullptr;
        pi.stageCount=geometry?3:2;pi.pStages=stages;pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;
        pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&samples;
        pi.pColorBlendState=&blend;pi.layout=layout;pi.renderPass=dynamic?VK_NULL_HANDLE:pass;
        const auto before=pi;const auto rendering_before=rendering;
        const auto stages_before=std::array<VkPipelineShaderStageCreateInfo,3>{stages[0],stages[1],stages[2]};
        CHECK(c.CreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
        if(memcmp(&before,&pi,sizeof(pi)) || memcmp(&rendering_before,&rendering,sizeof(rendering)) || memcmp(stages_before.data(),stages,sizeof(stages)))throw std::runtime_error("Application pipeline state mutated");
        if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}
        if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=c.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        CHECK(c.AllocateCommandBuffers(c.device,&ca,&image.command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        VkClearValue clear{};clear.color.float32[3]=1;
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=pass;rb.framebuffer=frame;rb.renderArea={{0,0},{W,H}};rb.clearValueCount=1;rb.pClearValues=&clear;
        if(dynamic) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=image.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachment.imageView=image.view;attachment.imageLayout=barrier.newLayout;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.clearValue=clear;
            VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea=rb.renderArea;info.layerCount=1;
            info.colorAttachmentCount=1;info.pColorAttachments=&attachment;
            auto begin_rendering=(PFN_vkCmdBeginRendering)c.gdpa(c.device,c.properties.apiVersion>=VK_API_VERSION_1_3?"vkCmdBeginRendering":"vkCmdBeginRenderingKHR");
            if(!begin_rendering)throw std::runtime_error("Missing dynamic rendering entry");
            begin_rendering(image.command,&info);
        } else c.CmdBeginRenderPass(image.command,&rb,VK_SUBPASS_CONTENTS_INLINE);
        c.CmdBindPipeline(image.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        c.CmdDraw(image.command,3,1,0,0);
        if(dynamic) {
            auto end_rendering=(PFN_vkCmdEndRendering)c.gdpa(c.device,c.properties.apiVersion>=VK_API_VERSION_1_3?"vkCmdEndRendering":"vkCmdEndRenderingKHR");
            if(!end_rendering)throw std::runtime_error("Missing dynamic rendering end");
            end_rendering(image.command);
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=image.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        } else c.CmdEndRenderPass(image.command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};c.CmdCopyImageToBuffer(image.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(c.EndCommandBuffer(image.command));VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&image.command;CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);
        auto pixels=(const uint8_t*)output.mapped;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const auto *p=pixels+(y*W+x)*4;bool red=variant==1 ? !c.original.shaderCullDistance : x>=W/2;if(p[0]!=(red?255:0)||p[1]||p[2]||p[3]!=255)return false;}
        return true;
    }
};
std::string run(const std::string &directory,const std::string &mode) {
    Context c;c.initialize(directory,mode!="distance-native",VK_API_VERSION_1_3);
    std::ostringstream log;log<<"Device: "<<c.properties.deviceName<<"; mode="<<mode<<"\n";
    if(!c.features.shaderClipDistance || !c.features.shaderCullDistance)
        return log.str()+"SKIP: ClipDistance and CullDistance are required\n";
    uint32_t count=0;
    auto enumerate=c.instance_proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    CHECK(enumerate(c.physical,nullptr,&count,nullptr));std::vector<VkExtensionProperties> extensions(count);
    CHECK(enumerate(c.physical,nullptr,&count,extensions.data()));
    bool core=c.properties.apiVersion>=VK_API_VERSION_1_3,available=core;
    for(auto &e:extensions)available|=!strcmp(e.extensionName,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if(available){query.pNext=&dynamic;c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical,&query);}
    const char *extension=VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    VkPhysicalDeviceFeatures enabled{};enabled.shaderClipDistance=enabled.shaderCullDistance=VK_TRUE;enabled.geometryShader=c.features.geometryShader;enabled.shaderTessellationAndGeometryPointSize=c.features.shaderTessellationAndGeometryPointSize;
    c.create_device(enabled,dynamic.dynamicRendering?(VkBaseOutStructure*)&dynamic:nullptr,!core&&dynamic.dynamicRendering?1:0,&extension);
    log<<"Native components: VS output="<<c.properties.limits.maxVertexOutputComponents
       <<" FS input="<<c.properties.limits.maxFragmentInputComponents
       <<" GS input="<<c.properties.limits.maxGeometryInputComponents
       <<" GS output="<<c.properties.limits.maxGeometryOutputComponents<<"\n";
    if(!c.original.shaderCullDistance)log<<"LIMITATION: MaliBridge demotes CullDistance without culling; expected full coverage\n";
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);unsigned cases=0,failed=0;
    for(unsigned variant=0;variant<6;++variant) {
        if(variant>=4 && !enabled.geometryShader){log<<"SKIP: native geometry stage unavailable\n";continue;}
        Graphics graphics(c,variant);
        for(unsigned d=0;d<2;++d) {
            if(d&&!dynamic.dynamicRendering)continue;
            bool pass=graphics.render(output,d,variant);++cases;if(!pass)++failed;
            log<<(pass?"PASS ":"FAIL ")<<"distance="<<variant<<" dynamic="<<d
               <<"; exact RGBA readback, caller state unchanged\n";
        }
    }
    if(!dynamic.dynamicRendering)log<<"SKIP: native dynamic rendering unavailable\n";
    log<<(failed?"FAIL: ":"PASS: ")<<cases<<" graphics cases, "<<failed<<" failures\n";
    return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="distance-native"&&mode!="distance-normal")return 2;
    try {
        result=run(argv[1],mode);fflush(stderr);
        if(mode=="distance-normal" && result.find("PASS: ")!=std::string::npos) {
            std::ifstream trace(std::string(argv[3])+"/trace.txt");std::string line;bool observed=false;
            while(std::getline(trace,line))observed|=line.find("HYBRIS_MALIBRIDGE_DISTANCE pipeline_base=")==0;
            if(!observed)throw std::runtime_error("MaliBridge pipeline lowering was not observed");
        }
    }catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
