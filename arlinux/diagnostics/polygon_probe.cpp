/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "polygon_vert.inc"
#include "polygon_frag.inc"
using namespace diagnostics;
namespace {
constexpr uint32_t W=32,H=32;
struct Graphics {
    Context &c;ImageResources image;
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer frame=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkDescriptorSetLayout sets=VK_NULL_HANDLE;VkDescriptorPool descriptors=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;
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
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ds.bindingCount=1;ds.pBindings=&binding;CHECK(c.CreateDescriptorSetLayout(c.device,&ds,nullptr,&sets));
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,16};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&sets;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;CHECK(c.CreatePipelineLayout(c.device,&li,nullptr,&layout));
        VkDescriptorPoolSize amount{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&amount;CHECK(c.CreateDescriptorPool(c.device,&pool,nullptr,&descriptors));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=descriptors;allocate.descriptorSetCount=1;allocate.pSetLayouts=&sets;CHECK(c.AllocateDescriptorSets(c.device,&allocate,&set));
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(polygon_vert_spv);sm.pCode=polygon_vert_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&vertex));
        sm.codeSize=sizeof(polygon_frag_spv);sm.pCode=polygon_frag_spv;CHECK(c.CreateShaderModule(c.device,&sm,nullptr,&fragment));
    }
    ~Graphics(){if(!c.idle)return;if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);if(vertex)c.DestroyShaderModule(c.device,vertex,nullptr);if(fragment)c.DestroyShaderModule(c.device,fragment,nullptr);if(descriptors)c.DestroyDescriptorPool(c.device,descriptors,nullptr);if(sets)c.DestroyDescriptorSetLayout(c.device,sets,nullptr);if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);if(frame)c.DestroyFramebuffer(c.device,frame,nullptr);if(pass)c.DestroyRenderPass(c.device,pass,nullptr);}
    VkPipeline make(VkPolygonMode mode,VkPrimitiveTopology topology,bool restart,int resolver) {
        VkPipelineShaderStageCreateInfo stages[2]{};for(auto &s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
        VkVertexInputBindingDescription binding{0,8,VK_VERTEX_INPUT_RATE_VERTEX};VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32G32_SFLOAT,0};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};input.vertexBindingDescriptionCount=1;input.pVertexBindingDescriptions=&binding;input.vertexAttributeDescriptionCount=1;input.pVertexAttributeDescriptions=&attribute;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=topology;assembly.primitiveRestartEnable=restart;
        VkViewport viewport{0,0,float(W),float(H),0,1};VkRect2D scissor{{0,0},{W,H}};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=mode;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=0xf;VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&color;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&samples;pi.pColorBlendState=&blend;pi.layout=layout;pi.renderPass=pass;
        auto before_raster=raster;auto before_assembly=assembly;
        auto create=(PFN_vkCreateGraphicsPipelines)(resolver?c.gipa(c.instance,"vkCreateGraphicsPipelines"):c.gdpa(c.device,"vkCreateGraphicsPipelines"));
        VkPipeline pipeline;CHECK(create(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
        if(memcmp(&raster,&before_raster,sizeof(raster))||memcmp(&assembly,&before_assembly,sizeof(assembly)))throw std::runtime_error("Caller pipeline state changed");return pipeline;
    }
    std::vector<uint8_t> render(bool reference,VkPolygonMode wanted,VkPrimitiveTopology topology,unsigned index_size,bool secondary,int upload,int resolver,bool restored=false,bool restart=false,bool rejected=false) {
        // The same geometry/vertex IDs are fed to both the emulated and explicit native reference paths.
        const std::array<float,2> positions[]={{{-0.71875f,-0.71875f}},{{0.71875f,-0.71875f}},{{-0.71875f,0.71875f}},{{0.71875f,0.71875f}}};
        std::vector<uint32_t> ordinals=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?std::vector<uint32_t>{0,1,2,2,1,3}:topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP?std::vector<uint32_t>{0,1,2,3}:std::vector<uint32_t>{0,1,3,2};
        if(restart)ordinals={0,1,2,UINT32_MAX,1,2,3};
        std::vector<float> vertices(32,0);for(unsigned i=0;i<(index_size?4:ordinals.size());++i) {
            unsigned pos=index_size?i:ordinals[i];vertices[(4+i)*2]=positions[pos][0];vertices[(4+i)*2+1]=positions[pos][1];}
        Buffer vb(c,vertices.size()*4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);memcpy(vb.mapped,vertices.data(),vertices.size()*4);vb.visibility(true);
        Buffer uniform(c,16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);const float translation[]={0.03125f,-0.03125f,0,0};memcpy(uniform.mapped,translation,16);uniform.visibility(true);
        VkDescriptorBufferInfo ub{uniform.handle,0,16};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&ub;c.UpdateDescriptorSets(c.device,1,&write,0,nullptr);
        const unsigned size=index_size?index_size:4;const unsigned count=ordinals.size();std::vector<uint8_t> bytes(4+(count+2)*size,0);
        for(unsigned i=0;i<count;++i){uint32_t value=ordinals[i]==UINT32_MAX?(size==2?0xffffu:UINT32_MAX):ordinals[i]+(restart?4:7);memcpy(bytes.data()+4+(i+2)*size,&value,size);}
        Buffer indices(c,bytes.size(),VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);memcpy(indices.mapped,bytes.data(),bytes.size());if(upload==2)memset(indices.mapped,0,bytes.size());indices.visibility(true);
        std::vector<uint32_t> expanded;
        if(wanted==VK_POLYGON_MODE_POINT)for(unsigned i=0;i<count;++i)expanded.push_back(index_size?ordinals[i]+(restart?4:7):i);
        else {unsigned tris=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?count/3:count-2;
            for(unsigned t=0;t<tris;++t){uint32_t a=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?t*3:topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP?t:0,b=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?t*3+1:t+1,d=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST?t*3+2:t+2;
                bool skip=ordinals[a]==UINT32_MAX||ordinals[b]==UINT32_MAX||ordinals[d]==UINT32_MAX;
                uint32_t v[]={a,b,b,d,d,a};for(unsigned x:v)expanded.push_back(skip?0:index_size?ordinals[x]+(restart?4:7):x);}}
        Buffer edges(c,expanded.size()*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);memcpy(edges.mapped,expanded.data(),expanded.size()*4);edges.visibility(true);
        Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkPolygonMode actual=reference?VK_POLYGON_MODE_FILL:wanted;VkPrimitiveTopology actual_topology=reference?(wanted==VK_POLYGON_MODE_POINT?VK_PRIMITIVE_TOPOLOGY_POINT_LIST:VK_PRIMITIVE_TOPOLOGY_LINE_LIST):topology;
        VkPipeline pipe=make(actual,actual_topology,reference?false:restart,resolver),filled=restored?make(VK_POLYGON_MODE_FILL,topology,false,0):VK_NULL_HANDLE;
        if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=c.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;CHECK(c.AllocateCommandBuffers(c.device,&allocate,&image.command));
        VkCommandBuffer sub=VK_NULL_HANDLE,init=VK_NULL_HANDLE;auto update=(PFN_vkCmdUpdateBuffer)c.gdpa(c.device,"vkCmdUpdateBuffer");auto execute=(PFN_vkCmdExecuteCommands)c.gdpa(c.device,"vkCmdExecuteCommands");auto bind_index=(PFN_vkCmdBindIndexBuffer)c.gdpa(c.device,"vkCmdBindIndexBuffer");auto bind_vertex=(PFN_vkCmdBindVertexBuffers)c.gdpa(c.device,"vkCmdBindVertexBuffers");auto indexed=(PFN_vkCmdDrawIndexed)c.gdpa(c.device,"vkCmdDrawIndexed");
        auto record=[&](VkCommandBuffer command){VkDeviceSize offset=0;c.CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);bind_vertex(command,0,1,&vb.handle,&offset);c.CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);float color[]={1,0.5,0,1};c.CmdPushConstants(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,color);
            if(reference){bind_index(command,edges.handle,0,VK_INDEX_TYPE_UINT32);indexed(command,expanded.size(),2,0,index_size?(restart?0:-3):4,2);}
            else if(index_size){bind_index(command,indices.handle,4,index_size==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);indexed(command,count,2,2,restart?0:-3,2);}
            else c.CmdDraw(command,count,2,4,2);
            if(restored){c.CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,filled);if(reference)bind_index(command,indices.handle,4,index_size==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);indexed(command,3,1,2,-3,2);}};
        if(secondary){allocate.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;CHECK(c.AllocateCommandBuffers(c.device,&allocate,&sub));VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inherit.renderPass=pass;inherit.framebuffer=frame;VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;begin.pInheritanceInfo=&inherit;CHECK(c.BeginCommandBuffer(sub,&begin));record(sub);CHECK(c.EndCommandBuffer(sub));}
        if(upload==2){allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;CHECK(c.AllocateCommandBuffers(c.device,&allocate,&init));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(init,&begin));update(init,indices.handle,0,bytes.size(),bytes.data());CHECK(c.EndCommandBuffer(init));}
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        if(upload==1)update(image.command,indices.handle,0,bytes.size(),bytes.data());
        if(upload){VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_INDEX_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&barrier,0,nullptr,0,nullptr);}
        VkClearValue clear{};clear.color.float32[3]=1;VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.framebuffer=frame;rp.renderArea={{0,0},{W,H}};rp.clearValueCount=1;rp.pClearValues=&clear;c.CmdBeginRenderPass(image.command,&rp,secondary?VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS:VK_SUBPASS_CONTENTS_INLINE);
        if(secondary)execute(image.command,1,&sub);else record(image.command);c.CmdEndRenderPass(image.command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={W,H,1};c.CmdCopyImageToBuffer(image.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.handle,1,&copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        VkResult end=c.EndCommandBuffer(image.command);
        if(rejected){if(end!=VK_ERROR_FEATURE_NOT_PRESENT)throw std::runtime_error("Primary/secondary index-write hazard was not rejected");}
        else {CHECK(end);VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));VkCommandBuffer commands[]={init,image.command};VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=init?2:1;submit.pCommandBuffers=init?commands:commands+1;CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);}
        std::vector<uint8_t> pixels((uint8_t*)output.mapped,(uint8_t*)output.mapped+W*H*4);
        auto reset_pool=(PFN_vkResetCommandPool)c.gdpa(c.device,"vkResetCommandPool");CHECK(reset_pool(c.device,c.pool,0));
        c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;if(sub)c.FreeCommandBuffers(c.device,c.pool,1,&sub);if(init)c.FreeCommandBuffers(c.device,c.pool,1,&init);
        c.DestroyPipeline(c.device,pipe,nullptr);if(filled)c.DestroyPipeline(c.device,filled,nullptr);return pixels;
    }
};
std::string run(const std::string &directory,const std::string &mode,bool features2) {
    setenv("HYBRIS_POLYGON_MODE",mode=="polygon-force"?"force":mode=="polygon-off"?"0":"1",1);
    Context c;c.initialize(directory,mode!="polygon-native");bool enabled=c.features.fillModeNonSolid;
    std::ostringstream log;log<<"Device: "<<c.properties.deviceName<<" native fillModeNonSolid="<<c.original.fillModeNonSolid<<" exposed="<<enabled<<" features2="<<features2<<"\n";
    VkPhysicalDeviceFeatures features{};features.fillModeNonSolid=enabled;VkPhysicalDeviceFeatures2 create{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};create.features=features;c.create_device(features,features2?(VkBaseOutStructure*)&create:nullptr,0,nullptr,features2);
    Graphics graphics(c);unsigned checks=0,failed=0;
    auto check=[&](bool pass,const std::string &what){++checks;if(!pass)++failed;log<<(pass?"PASS ":"FAIL ")<<what<<"\n";};
    for(VkPolygonMode polygon:{VK_POLYGON_MODE_LINE,VK_POLYGON_MODE_POINT})for(VkPrimitiveTopology topology:{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN})for(unsigned isize:{0u,2u,4u})for(bool secondary:{false,true})for(int resolver=0;resolver<2;++resolver) {
        auto reference=graphics.render(true,polygon,topology,isize,secondary,0,resolver);auto actual=graphics.render(!enabled,polygon,topology,isize,secondary,0,resolver);
        unsigned visible=0;for(unsigned i=0;i<reference.size();i+=4)visible+=reference[i]!=0;
        check(reference==actual && visible>0 && visible<W*H/2,"mode="+std::to_string(polygon)+" topology="+std::to_string(topology)+" index="+std::to_string(isize)+" secondary="+std::to_string(secondary)+" resolver="+std::to_string(resolver)+" exact native-reference pixels");
    }
    if(enabled && c.layered && mode!="polygon-off") {
        auto reference=graphics.render(true,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,2,0);
        auto actual=graphics.render(false,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,2,0);
        check(actual==reference,"GPU upload earlier in the same submission precedes index prepass");
        reference=graphics.render(true,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,0,0,true);actual=graphics.render(false,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,0,0,true);
        check(actual==reference,"original index binding restored for following filled draw");
        reference=graphics.render(true,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,2,false,0,0,false,true);actual=graphics.render(false,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,2,false,0,0,false,true);
        check(actual==reference,"16-bit strip restart boundaries");
        auto solid=graphics.render(false,VK_POLYGON_MODE_FILL,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,1,0);
        auto fallback=graphics.render(false,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,1,0);
        auto lines=graphics.render(true,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,false,0,0);
        check(fallback==solid && fallback!=lines,"same-recording index update chooses complete filled fallback");
        graphics.render(false,VK_POLYGON_MODE_LINE,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,4,true,1,0,false,false,true);check(true,"primary index write before indexed secondary explicitly rejected");
    }
    log<<(failed?"FAIL: ":"PASS: ")<<checks<<" GPU/API checks, "<<failed<<" failed. Offscreen polygon paths only; culling/depth bias and unsupported draws retain documented limitations.\n";return log.str();
}
}
int main(int argc,char **argv){if(argc!=4)return 2;std::string mode=argv[2],result;if(mode!="polygon-native"&&mode!="polygon-normal"&&mode!="polygon-force"&&mode!="polygon-off")return 2;try{if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables policy overrides");result=run(argv[1],mode,false)+run(argv[1],mode,true);}catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}return report_result(result);}
