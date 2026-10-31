/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "multidraw_vert.inc"
#include "multidraw_drawid_vert.inc"
#include "multidraw_frag.inc"
using namespace diagnostics;
namespace {
constexpr uint32_t W=48,H=16;
struct Graphics {
    Context &c;ImageResources image;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer frame=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    Graphics(Context &context,bool drawid):c(context),image(c) {
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
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=drawid?sizeof(multidraw_drawid_vert_spv):sizeof(multidraw_vert_spv);sm.pCode=drawid?multidraw_drawid_vert_spv:multidraw_vert_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&vertex));
        sm.codeSize=sizeof(multidraw_frag_spv);sm.pCode=multidraw_frag_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&fragment));
    }
    ~Graphics(){if(!c.idle)return;if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);if(vertex)c.DestroyShaderModule(c.device,vertex,nullptr);if(fragment)c.DestroyShaderModule(c.device,fragment,nullptr);if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);if(frame)c.DestroyFramebuffer(c.device,frame,nullptr);if(pass)c.DestroyRenderPass(c.device,pass,nullptr);}
    bool render(Buffer &output,bool indexed,unsigned count,unsigned stride,int resolver,bool native_reference,bool drawid) {
        if(pipeline){c.DestroyPipeline(c.device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        VkViewport viewport{0,0,float(W),float(H),0,1};VkRect2D scissor{{0,0},{W,H}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(auto &s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=0xf;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&color;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&samples;pi.pColorBlendState=&blend;pi.layout=layout;pi.renderPass=pass;
        CHECK(c.CreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
        std::vector<uint8_t> parameters(16+stride*3,0xa5);
        for(unsigned i=0;i<3;++i) {
            if(indexed){VkDrawIndexedIndirectCommand d{6,1,3,-3,i};memcpy(parameters.data()+16+stride*i,&d,sizeof(d));}
            else {VkDrawIndirectCommand d{6,1,6,i};memcpy(parameters.data()+16+stride*i,&d,sizeof(d));}
        }
        Buffer indirect(c,parameters.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        Buffer recovered(c,parameters.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer indices(c,40,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        const uint32_t index_values[]={0,0,0,0,9,10,11,12,13,14};memcpy(indices.mapped,index_values,sizeof(index_values));indices.visibility(true);
        if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}
        if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=c.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        CHECK(c.AllocateCommandBuffers(c.device,&ca,&image.command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        auto update=(PFN_vkCmdUpdateBuffer)c.gdpa(c.device,"vkCmdUpdateBuffer");auto copy_buffer=(PFN_vkCmdCopyBuffer)c.gdpa(c.device,"vkCmdCopyBuffer");auto bind_index=(PFN_vkCmdBindIndexBuffer)c.gdpa(c.device,"vkCmdBindIndexBuffer");
        if(!update||!copy_buffer||!bind_index)throw std::runtime_error("Indirect probe entry missing");
        update(image.command,indirect.handle,0,parameters.size(),parameters.data());
        VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER};visibility.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;visibility.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&visibility,0,nullptr,0,nullptr);
        VkClearValue clear{};clear.color.float32[3]=1;VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=pass;rb.framebuffer=frame;rb.renderArea={{0,0},{W,H}};rb.clearValueCount=1;rb.pClearValues=&clear;
        c.CmdBeginRenderPass(image.command,&rb,VK_SUBPASS_CONTENTS_INLINE);c.CmdBindPipeline(image.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        if(indexed)bind_index(image.command,indices.handle,4,VK_INDEX_TYPE_UINT32);
        const char *name=indexed?"vkCmdDrawIndexedIndirect":"vkCmdDrawIndirect";
        auto function=resolver?c.gipa(c.instance,name):c.gdpa(c.device,name);if(!function)throw std::runtime_error(std::string("Missing ")+name);
        auto issue=[&](VkDeviceSize offset,unsigned n){
            if(indexed)((PFN_vkCmdDrawIndexedIndirect)function)(image.command,indirect.handle,offset,n,stride);
            else ((PFN_vkCmdDrawIndirect)function)(image.command,indirect.handle,offset,n,stride);
        };
        if(native_reference && count>1)for(unsigned i=0;i<count;++i)issue(16+stride*i,1);
        else issue(16,count);
        c.CmdEndRenderPass(image.command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};c.CmdCopyImageToBuffer(image.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        VkBufferCopy preserve{0,0,parameters.size()};copy_buffer(image.command,indirect.handle,recovered.handle,1,&preserve);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(c.EndCommandBuffer(image.command));VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&image.command;CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);recovered.visibility(false);
        if(memcmp(parameters.data(),recovered.mapped,parameters.size()))throw std::runtime_error("Indirect buffer contents changed");
        auto pixels=(const uint8_t*)output.mapped;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
            unsigned column=x/(W/3);bool painted=drawid?(count>0 && column==0):column<count;
            auto pixel=pixels+(y*W+x)*4;
            for(unsigned channel=0;channel<4;++channel) {
                unsigned expected=channel==3?255:painted&&channel==column?255:0;
                if(pixel[channel]!=expected){fprintf(stderr,"PIXEL mismatch x=%u y=%u channel=%u got=%u want=%u\n",x,y,channel,pixel[channel],expected);return false;}
            }
        }
        return true;
    }
};
std::string run(const std::string &directory,const std::string &mode,bool features2) {
    setenv("HYBRIS_MULTI_DRAW_INDIRECT",mode=="indirect-force"?"force":mode=="indirect-off"?"0":"1",1);
    Context c;c.initialize(directory,mode!="indirect-native");
    bool fallback=c.layered && mode!="indirect-off" && (mode=="indirect-force"||!c.original.multiDrawIndirect);
    bool requested=mode!="indirect-unrequested" && c.features.multiDrawIndirect;
    std::ostringstream log;log<<"Device: "<<c.properties.deviceName<<"; native multiDrawIndirect="<<c.original.multiDrawIndirect<<" exposed="<<c.features.multiDrawIndirect<<" maxDrawIndirectCount="<<c.properties.limits.maxDrawIndirectCount<<" requested="<<requested<<" features2="<<features2<<"\n";
    if(fallback && (!c.features.multiDrawIndirect||c.properties.limits.maxDrawIndirectCount!=UINT32_MAX))throw std::runtime_error("Indirect compatibility capability missing");
    if(!fallback && c.features.multiDrawIndirect!=c.original.multiDrawIndirect)throw std::runtime_error("Native feature changed");
    VkPhysicalDeviceFeatures enabled{};enabled.multiDrawIndirect=requested;enabled.drawIndirectFirstInstance=c.original.drawIndirectFirstInstance;
    if(!enabled.drawIndirectFirstInstance)return "UNSUPPORTED: native nonzero indirect firstInstance unavailable\n";
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&draw_parameters;c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical,&query);
    VkPhysicalDeviceFeatures2 create{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};create.features=enabled;create.pNext=&draw_parameters;
    const auto before=enabled;const auto before2=create.features;
    c.create_device(enabled,features2?(VkBaseOutStructure*)&create:(VkBaseOutStructure*)&draw_parameters,0,nullptr,features2);
    if(memcmp(&before,&enabled,sizeof(enabled))||memcmp(&before2,&create.features,sizeof(enabled)))throw std::runtime_error("Caller feature state changed");
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);unsigned checks=0,failed=0;
    {
        Graphics graphics(c,false);
        for(bool indexed:{false,true})for(bool padded:{false,true})for(unsigned count:{0u,1u,3u})for(int resolver=0;resolver<2;++resolver) {
            unsigned stride=(indexed?sizeof(VkDrawIndexedIndirectCommand):sizeof(VkDrawIndirectCommand))+(padded?16:0);
            bool reference=!requested;bool pass=graphics.render(output,indexed,count,stride,resolver,reference,false);++checks;if(!pass)++failed;
            log<<(pass?"PASS ":"FAIL ")<<"indexed="<<indexed<<" count="<<count<<" stride="<<stride<<" resolver="<<(resolver?"GIPA":"GDPA")<<" reference="<<reference<<" exact RGBA and indirect-buffer readback\n";
        }
    }
    if(draw_parameters.shaderDrawParameters && (!requested || fallback)) {
        Graphics drawid(c,true);bool pass=drawid.render(output,false,3,sizeof(VkDrawIndirectCommand),0,!requested,true);++checks;if(!pass)++failed;
        log<<(pass?"LIMITATION ":"FAIL ")<<"DrawIndex: single-draw calls all observe zero; multi-draw shader DrawIndex is not emulated\n";
    }
    log<<(failed?"FAIL: ":"PASS: ")<<checks<<" GPU checks; "<<failed<<" failures. Targeted compatibility, not Vulkan conformance or game acceptance.\n";
    return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="indirect-native"&&mode!="indirect-normal"&&mode!="indirect-force"&&mode!="indirect-off"&&mode!="indirect-unrequested")return 2;
    try {
        if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables policy overrides");
        result=run(argv[1],mode,false)+run(argv[1],mode,true);
        fflush(stderr);std::ifstream trace(std::string(argv[3])+"/trace.txt");std::string line;unsigned seen=0;
        while(std::getline(trace,line)){unsigned indexed;if(sscanf(line.c_str(),"HYBRIS_MULTI_DRAW_INDIRECT indexed=%u",&indexed)==1)seen|=1u<<indexed;}
        if(mode=="indirect-force"&&seen!=3)result+="FAIL: both split paths were not observed\n";
        if((mode=="indirect-native"||mode=="indirect-off"||mode=="indirect-unrequested")&&seen)result+="FAIL: inactive route split a draw\n";
    }catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
