/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "viewport_vert.inc"
#include "dual_single_frag.inc"
#include "dual_pair_frag.inc"
#include "dual_extra_frag.inc"
#include "dual_full_frag.inc"
#include "dual_array_frag.inc"

using namespace diagnostics;
namespace {
constexpr uint32_t W=32,H=16;
struct Graphics {
    Context &c;ImageResources image;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer frame=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    std::vector<uint32_t> original;
    Graphics(Context &context,int variant):c(context),image(c) {
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
        const uint32_t *codes[]={dual_single_frag_spv,dual_pair_frag_spv,dual_extra_frag_spv,dual_full_frag_spv,dual_array_frag_spv};
        size_t sizes[]={sizeof(dual_single_frag_spv),sizeof(dual_pair_frag_spv),sizeof(dual_extra_frag_spv),sizeof(dual_full_frag_spv),sizeof(dual_array_frag_spv)};
        original.assign(codes[variant],codes[variant]+sizes[variant]/4);sm.codeSize=sizes[variant];sm.pCode=original.data();CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&fragment));
    }
    ~Graphics(){if(!c.idle)return;if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);if(vertex)c.DestroyShaderModule(c.device,vertex,nullptr);if(fragment)c.DestroyShaderModule(c.device,fragment,nullptr);if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);if(frame)c.DestroyFramebuffer(c.device,frame,nullptr);if(pass)c.DestroyRenderPass(c.device,pass,nullptr);}
    bool render(Buffer &output,unsigned factor,bool destination,bool enabled,bool source1,bool native_dual,int resolver,bool reject=false) {
        if(pipeline){c.DestroyPipeline(c.device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        const VkBlendFactor first[]={VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
        const VkBlendFactor second[]={VK_BLEND_FACTOR_SRC1_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,VK_BLEND_FACTOR_SRC1_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA};
        VkBlendFactor f=source1?second[factor]:first[factor];
        VkViewport viewport{0,0,float(W),float(H),0,1};VkRect2D scissor{{0,0},{W,H}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(auto &s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};color.blendEnable=enabled;color.colorWriteMask=0xf;
        color.srcColorBlendFactor=color.srcAlphaBlendFactor=destination?VK_BLEND_FACTOR_ZERO:f;
        color.dstColorBlendFactor=color.dstAlphaBlendFactor=destination?f:VK_BLEND_FACTOR_ZERO;
        color.colorBlendOp=color.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&color;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&samples;pi.pColorBlendState=&blend;pi.layout=layout;pi.renderPass=pass;
        auto before=color;auto original_code=original;
        auto create=(PFN_vkCreateGraphicsPipelines)(resolver?c.gipa(c.instance,"vkCreateGraphicsPipelines"):c.gdpa(c.device,"vkCreateGraphicsPipelines"));
        if(!create)throw std::runtime_error("Missing graphics pipeline entry");
        VkResult result=create(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline);
        if(memcmp(&before,&color,sizeof(color)) || original_code!=original)throw std::runtime_error("Caller shader/blend state changed");
        if(reject)return result==VK_ERROR_FEATURE_NOT_PRESENT && pipeline==VK_NULL_HANDLE;
        CHECK(result);
        if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}
        if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=c.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        CHECK(c.AllocateCommandBuffers(c.device,&ca,&image.command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        VkClearValue clear{};const float target[]={26/255.0f,51/255.0f,77/255.0f,102/255.0f};memcpy(clear.color.float32,target,sizeof(target));
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=pass;rb.framebuffer=frame;rb.renderArea={{0,0},{W,H}};rb.clearValueCount=1;rb.pClearValues=&clear;
        c.CmdBeginRenderPass(image.command,&rb,VK_SUBPASS_CONTENTS_INLINE);c.CmdBindPipeline(image.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);c.CmdDraw(image.command,3,1,0,0);c.CmdEndRenderPass(image.command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};c.CmdCopyImageToBuffer(image.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(c.EndCommandBuffer(image.command));VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&image.command;CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);
        const float src0[]={0.25f,0.5f,0.75f,0.5f},src1[]={0.75f,0.25f,0.5f,0.25f};const float *source=native_dual?src1:src0;
        auto pixels=(const uint8_t*)output.mapped;
        for(unsigned i=0;i<W*H;++i)for(unsigned channel=0;channel<4;++channel) {
            float f=source[factor>=2?3:channel];if(factor&1)f=1-f;
            int expected=std::lround(255*(enabled?(destination?target[channel]:src0[channel])*f:src0[channel]));
            if(abs(int(pixels[i*4+channel])-expected)>1){fprintf(stderr,"PIXEL channel=%u got=%u want=%d\n",channel,pixels[i*4+channel],expected);return false;}
        }
        return true;
    }
};
std::string run(const std::string &directory,const std::string &mode,bool features2) {
    setenv("HYBRIS_DUAL_SOURCE_BLEND",mode=="dual-force"?"force":mode=="dual-off"?"0":"1",1);
    Context c;c.initialize(directory,mode!="dual-native");
    bool fallback=c.layered && mode!="dual-off" && (mode=="dual-force"||!c.original.dualSrcBlend);
    bool requested=mode!="dual-unrequested" && c.features.dualSrcBlend;
    bool native_dual=requested&&!fallback;
    std::ostringstream log;log<<"Device: "<<c.properties.deviceName<<"; native dualSrcBlend="<<c.original.dualSrcBlend<<" exposed="<<c.features.dualSrcBlend<<" maxFragmentDualSrcAttachments="<<c.properties.limits.maxFragmentDualSrcAttachments<<" requested="<<requested<<" features2="<<features2<<"\n";
    if(fallback && (!c.features.dualSrcBlend||c.properties.limits.maxFragmentDualSrcAttachments<1))throw std::runtime_error("Dual-source compatibility missing");
    if(!fallback && c.features.dualSrcBlend!=c.original.dualSrcBlend)throw std::runtime_error("Native feature changed");
    VkPhysicalDeviceFeatures enabled{};enabled.dualSrcBlend=requested;VkPhysicalDeviceFeatures2 create{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};create.features=enabled;
    const auto before=enabled;const auto before2=create.features;c.create_device(enabled,features2?(VkBaseOutStructure*)&create:nullptr,0,nullptr,features2);
    if(memcmp(&before,&enabled,sizeof(enabled))||memcmp(&before2,&create.features,sizeof(enabled)))throw std::runtime_error("Caller feature state changed");
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);unsigned checks=0,failed=0;
    auto record=[&](bool pass,const std::string &name){++checks;if(!pass)++failed;log<<(pass?"PASS ":"FAIL ")<<name<<"; caller inputs unchanged\n";};
    {
        Graphics graphics(c,requested?1:0);
        for(unsigned factor=0;factor<4;++factor)for(bool destination:{false,true})for(int resolver=0;resolver<2;++resolver)
            record(graphics.render(output,factor,destination,true,requested,native_dual,resolver),"factor="+std::to_string(factor)+" destination="+std::to_string(destination)+" resolver="+(resolver?"GIPA":"GDPA")+" RGBA readback");
        record(graphics.render(output,0,false,false,requested,native_dual,0),"blending disabled, ordinary first output");
    }
    if(fallback&&requested) {
        Graphics extra(c,2);record(extra.render(output,2,false,true,true,false,0),"secondary output reserves a location past existing auxiliary output");
        if(c.properties.limits.maxFragmentOutputAttachments==8) {
            Graphics full(c,3);record(full.render(output,0,false,true,true,false,0,true),"no unused fragment location explicitly rejected");
        }
        Graphics array(c,4);record(array.render(output,0,false,true,true,false,0,true),"complex array output explicitly rejected");
        log<<"LIMITATION: SRC1 factors use SRC0; true dual-source pixels can differ. No PanVK driver-level blend shader is available here.\n";
    }
    log<<(failed?"FAIL: ":"PASS: ")<<checks<<" GPU/API checks; "<<failed<<" failures. Targeted approximation, not game acceptance or Vulkan conformance.\n";
    return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="dual-native"&&mode!="dual-normal"&&mode!="dual-force"&&mode!="dual-off"&&mode!="dual-unrequested")return 2;
    try {
        if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables policy overrides");
        result=run(argv[1],mode,false)+run(argv[1],mode,true);
        fflush(stderr);std::ifstream trace(std::string(argv[3])+"/trace.txt");std::string line;unsigned seen=0;
        while(std::getline(trace,line)){unsigned category;if(sscanf(line.c_str(),"HYBRIS_DUAL_SOURCE_BLEND category=%u",&category)==1)seen|=1u<<category;}
        if(mode=="dual-force"&&seen!=3)result+="FAIL: both dual-source rewrites were not observed\n";
        if((mode=="dual-native"||mode=="dual-off"||mode=="dual-unrequested")&&seen)result+="FAIL: inactive route rewrote dual-source state\n";
    }catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
