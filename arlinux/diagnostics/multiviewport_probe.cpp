/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "viewport_vert.inc"
#include "viewport_frag.inc"
using namespace diagnostics;
namespace {
constexpr uint32_t W=32,H=16;
struct Graphics {
    Context &c;ImageResources image;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer frame=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    Graphics(Context &context):c(context),image(c) {
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
    ~Graphics(){if(!c.idle)return;if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);if(vertex)c.DestroyShaderModule(c.device,vertex,nullptr);if(fragment)c.DestroyShaderModule(c.device,fragment,nullptr);if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);if(frame)c.DestroyFramebuffer(c.device,frame,nullptr);if(pass)c.DestroyRenderPass(c.device,pass,nullptr);}
    bool render(Buffer &output,uint32_t count,int dynamic,int resolver,bool reject_later) {
        if(pipeline){c.DestroyPipeline(c.device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        VkViewport views[]={{0,0,W/2.0f,float(H),0,1},{W/2.0f,0,W/2.0f,float(H),0,1}};
        VkRect2D scissors[]={{{0,0},{W,H/2}},{{0,H/2},{W,H/2}}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=dynamic>=2?0:count;vp.pViewports=views;vp.pScissors=scissors;
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
        VkDynamicState states[]={dynamic>=2?VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:VK_DYNAMIC_STATE_VIEWPORT,dynamic>=2?VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dyn.dynamicStateCount=2;dyn.pDynamicStates=states;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&samples;pi.pColorBlendState=&blend;pi.pDynamicState=dynamic?&dyn:nullptr;pi.layout=layout;pi.renderPass=pass;
        CHECK(c.CreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
        if(memcmp(&before,&vp,sizeof(vp)))throw std::runtime_error("Application pipeline state mutated");
        if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}
        if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=c.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        CHECK(c.AllocateCommandBuffers(c.device,&ca,&image.command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        VkClearValue clear{};clear.color.float32[3]=1;
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=pass;rb.framebuffer=frame;rb.renderArea={{0,0},{W,H}};rb.clearValueCount=1;rb.pClearValues=&clear;
        c.CmdBeginRenderPass(image.command,&rb,VK_SUBPASS_CONTENTS_INLINE);c.CmdBindPipeline(image.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        auto proc=[&](const char *name){auto p=resolver?c.gipa(c.instance,name):c.gdpa(c.device,name);if(!p)throw std::runtime_error(std::string("Missing ")+name);return p;};
        if(dynamic==1) {
            auto setv=(PFN_vkCmdSetViewport)proc("vkCmdSetViewport");auto sets=(PFN_vkCmdSetScissor)proc("vkCmdSetScissor");
            setv(image.command,0,count,views);sets(image.command,0,count,scissors);
            if(count>1){setv(image.command,1,1,views+1);sets(image.command,1,1,scissors+1);}
            // In compatibility mode index-one updates must be ignored, including their payload.
            if(reject_later){setv(image.command,1,1,views);sets(image.command,1,1,scissors);}
        } else if(dynamic>=2) {
            auto setv=(PFN_vkCmdSetViewportWithCount)proc(dynamic==3?"vkCmdSetViewportWithCountEXT":"vkCmdSetViewportWithCount");
            auto sets=(PFN_vkCmdSetScissorWithCount)proc(dynamic==3?"vkCmdSetScissorWithCountEXT":"vkCmdSetScissorWithCount");
            setv(image.command,count,views);sets(image.command,count,scissors);
        }
        c.CmdDraw(image.command,3,1,0,0);c.CmdEndRenderPass(image.command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};c.CmdCopyImageToBuffer(image.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(c.EndCommandBuffer(image.command));VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&image.command;CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);
        auto pixels=(const uint8_t*)output.mapped;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const auto *p=pixels+(y*W+x)*4;bool red=x<W/2 && y<H/2;if(p[0]!=(red?255:0)||p[1]||p[2]||p[3]!=255)return false;}
        return true;
    }
};
std::string run(const std::string &directory,const std::string &mode,bool features2) {
    setenv("HYBRIS_MULTI_VIEWPORT",mode=="viewport-force"?"force":mode=="viewport-off"?"0":"1",1);
    Context c;c.initialize(directory,mode!="viewport-native",VK_API_VERSION_1_3);
    std::ostringstream log;log<<"Device: "<<c.properties.deviceName<<"; native multiViewport="<<c.original.multiViewport<<" exposed="<<c.features.multiViewport<<" maxViewports="<<c.properties.limits.maxViewports<<"; features2="<<features2<<"\n";
    bool fallback=c.layered && mode!="viewport-off" && (mode=="viewport-force"||!c.original.multiViewport);
    if(fallback && (!c.features.multiViewport || c.properties.limits.maxViewports<16))throw std::runtime_error("Compatibility capability missing");
    if(!fallback && (c.features.multiViewport!=c.original.multiViewport))throw std::runtime_error("Native capability changed");
    uint32_t families=0;c.instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(c.physical,&families,nullptr);std::vector<VkQueueFamilyProperties> queues(families);c.instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(c.physical,&families,queues.data());
    if(!(queues[c.family].queueFlags&VK_QUEUE_GRAPHICS_BIT))throw std::runtime_error("Selected queue lacks graphics");
    uint32_t ext_count=0;auto enumerate=c.instance_proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");CHECK(enumerate(c.physical,nullptr,&ext_count,nullptr));std::vector<VkExtensionProperties> extensions(ext_count);CHECK(enumerate(c.physical,nullptr,&ext_count,extensions.data()));
    bool ext=false;for(auto &e:extensions)ext|=!strcmp(e.extensionName,VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    bool core=c.properties.apiVersion>=VK_API_VERSION_1_3;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT state{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&state;c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical,&query);
    bool with_count=core|| (ext&&state.extendedDynamicState);
    VkPhysicalDeviceFeatures enabled{};enabled.multiViewport=c.features.multiViewport;
    VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};requested.features=enabled;
    const char *extension_names[]={VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};
    if(features2){requested.pNext=with_count&&!core?&state:nullptr;c.create_device(enabled,(VkBaseOutStructure*)&requested,ext&&with_count?1:0,ext&&with_count?extension_names:nullptr,true);}
    else c.create_device(enabled,with_count&&!core?(VkBaseOutStructure*)&state:nullptr,ext&&with_count?1:0,ext&&with_count?extension_names:nullptr);
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);Graphics graphics(c);unsigned cases=0,failed=0;
    for(unsigned count=1;count<=(enabled.multiViewport?2u:1u);++count)for(int dynamic=0;dynamic<4;++dynamic)for(int resolver=0;resolver<2;++resolver) {
        if(dynamic>=2 && !with_count)continue;if(dynamic==3&&!ext)continue;if(!dynamic && resolver)continue;
        bool pass=graphics.render(output,count,dynamic,resolver,fallback);++cases;if(!pass)++failed;
        log<<(pass?"PASS ":"FAIL ")<<"count="<<count<<" state="<<dynamic<<" resolver="<<(resolver?"GIPA":"GDPA")<<" exact RGBA readback; input unchanged\n";
    }
    log<<(failed?"FAIL: ":"PASS: ")<<cases<<" graphics cases; "<<failed<<" failures. Only viewport zero rendered; shader ViewportIndex is NOT emulated.\n";
    return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="viewport-native"&&mode!="viewport-normal"&&mode!="viewport-force"&&mode!="viewport-off")return 2;
    try {
        if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables policy overrides");
        result=run(argv[1],mode,false)+run(argv[1],mode,true);
        fflush(stderr);std::ifstream trace(std::string(argv[3])+"/trace.txt");std::string line;unsigned seen=0;
        while(std::getline(trace,line)){unsigned category;if(sscanf(line.c_str(),"HYBRIS_MULTI_VIEWPORT category=%u",&category)==1)seen|=1u<<category;}
        if(mode=="viewport-force" && (seen&15)!=15)result+="FAIL: static/dynamic first-viewport clamping not observed\n";
        if((mode=="viewport-native"||mode=="viewport-off")&&seen)result+="FAIL: disabled compatibility changed state\n";
    }catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
