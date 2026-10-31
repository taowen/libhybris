/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "sample.inc"
#include "fixtures.h"
using namespace diagnostics;
namespace {
constexpr uint32_t sample_size=32;
std::vector<uint32_t> preview(256*256,0xff202830u);
struct BcContext:Context {
    bool native_bc=false,hdr_target=false;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    void initialize(const std::string &directory,bool compat) {
        Context::initialize(directory,compat);
        native_bc=original.textureCompressionBC;
        VkPhysicalDeviceTextureCompressionASTCHDRFeatures hdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES};
        VkPhysicalDeviceFeatures2 extended{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&hdr};
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(native_gipa(instance,"vkGetPhysicalDeviceFeatures2"))(physical,&extended);
        VkImageFormatProperties hdr_limits{};
        auto native_image=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(native_gipa(instance,"vkGetPhysicalDeviceImageFormatProperties"));
        hdr_target=hdr.textureCompressionASTC_HDR && native_image(physical,VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK,VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&hdr_limits)==VK_SUCCESS;
        VkPhysicalDeviceFeatures enabled{};enabled.textureCompressionBC=features.textureCompressionBC;
        create_device(enabled);
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0]={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        bindings[1]={1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sets.bindingCount=2;sets.pBindings=bindings;
        CHECK(CreateDescriptorSetLayout(device,&sets,nullptr,&set_layout));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo layouts{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layouts.setLayoutCount=1;layouts.pSetLayouts=&set_layout;layouts.pushConstantRangeCount=1;layouts.pPushConstantRanges=&range;
        CHECK(CreatePipelineLayout(device,&layouts,nullptr,&layout));
        VkShaderModuleCreateInfo shader_create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_create.codeSize=sizeof(sample_spv);shader_create.pCode=sample_spv;
        VkShaderModule shader=VK_NULL_HANDLE;
        CHECK(CreateShaderModule(device,&shader_create,nullptr,&shader));
        VkComputePipelineCreateInfo pipeline_create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        pipeline_create.layout=layout;
        VkResult result=CreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_create,nullptr,&pipeline);
        DestroyShaderModule(device,shader,nullptr);
        CHECK(result);
    }
    ~BcContext() {
        if(!idle)return;
        if(pipeline)DestroyPipeline(device,pipeline,nullptr);
        if(layout)DestroyPipelineLayout(device,layout,nullptr);
        if(set_layout)DestroyDescriptorSetLayout(device,set_layout,nullptr);
    }
};
struct Result {std::vector<float> sampled;bool raw_equal=false;double milliseconds=0;};
Result sample(BcContext &c,const Fixture &fixture,uint32_t size) {
    auto started=std::chrono::steady_clock::now();
    size_t bytes=static_cast<size_t>(size/4)*(size/4)*fixture.bytes;
    Buffer upload(c,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buffer readback(c,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer output(c,sample_size*sample_size*4*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for(size_t i=0;i<bytes;i+=fixture.bytes)memcpy(static_cast<char *>(upload.mapped)+i,fixture.block,fixture.bytes);
    upload.visibility(true);
    ImageResources r(c);
    VkImageCreateInfo image_create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_create.imageType=VK_IMAGE_TYPE_2D;image_create.format=fixture.format;image_create.extent={size,size,1};
    image_create.mipLevels=1;image_create.arrayLayers=1;image_create.samples=VK_SAMPLE_COUNT_1_BIT;
    image_create.tiling=VK_IMAGE_TILING_OPTIMAL;
    image_create.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    CHECK(c.CreateImage(c.device,&image_create,nullptr,&r.image));
    VkMemoryRequirements need;c.GetImageMemoryRequirements(c.device,r.image,&need);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=need.size;
    allocate.memoryTypeIndex=c.memory_type(need.memoryTypeBits,0);
    CHECK(c.AllocateMemory(c.device,&allocate,nullptr,&r.memory));
    CHECK(c.BindImageMemory(c.device,r.image,r.memory,0));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=r.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    view.format=fixture.format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    CHECK(c.CreateImageView(c.device,&view,nullptr,&r.view));
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter=VK_FILTER_NEAREST;sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(c.CreateSampler(c.device,&sampler,nullptr,&r.sampler));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
    CHECK(c.CreateDescriptorPool(c.device,&pool,nullptr,&r.descriptors));
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_allocate.descriptorPool=r.descriptors;set_allocate.descriptorSetCount=1;set_allocate.pSetLayouts=&c.set_layout;
    VkDescriptorSet set;CHECK(c.AllocateDescriptorSets(c.device,&set_allocate,&set));
    VkDescriptorImageInfo texture{r.sampler,r.view,VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo buffer{output.handle,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;}
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&texture;
    writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&buffer;
    c.UpdateDescriptorSets(c.device,2,writes,0,nullptr);
    VkCommandBufferAllocateInfo command_allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate.commandPool=c.pool;command_allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command_allocate.commandBufferCount=1;
    CHECK(c.AllocateCommandBuffers(c.device,&command_allocate,&r.command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(c.BeginCommandBuffer(r.command,&begin));
    // Bind before the upload: the decoder must restore the application's
    // compute pipeline, descriptors and push constants before our dispatch.
    c.CmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,c.pipeline);
    c.CmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,c.layout,0,1,&set,0,nullptr);
    uint32_t shape[]={size,sample_size};
    c.CmdPushConstants(r.command,c.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(shape),shape);
    VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    image_barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image_barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;image_barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    image_barrier.srcQueueFamilyIndex=image_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image=r.image;image_barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    c.CmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image_barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={size,size,1};
    c.CmdCopyBufferToImage(r.command,upload.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    image_barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    c.CmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image_barrier);
    c.CmdDispatch(r.command,sample_size/8,sample_size/8,1);
    c.CmdCopyImageToBuffer(r.command,r.image,VK_IMAGE_LAYOUT_GENERAL,readback.handle,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    c.CmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    CHECK(c.EndCommandBuffer(r.command));
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&r.fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    CHECK(c.QueueSubmit(c.queue,1,&submit,r.fence));
    c.idle=false;
    CHECK(c.WaitForFences(c.device,1,&r.fence,VK_TRUE,UINT64_C(10000000000)));
    c.idle=true;
    output.visibility(false);readback.visibility(false);
    Result result;
    result.sampled.assign(static_cast<float *>(output.mapped),static_cast<float *>(output.mapped)+sample_size*sample_size*4);
    result.raw_equal=memcmp(upload.mapped,readback.mapped,bytes)==0;
    result.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return result;
}
float reference(const Fixture &fixture,uint32_t pixel,uint32_t channel) {
    return fixture.reference[(pixel%16)*4+channel];
}


std::string run_probe(const std::string &directory,const std::string &mode,uint32_t size) {
    const bool compat=mode!="native";
    setenv("HYBRIS_BC_TEXTURES",mode=="normal"?"missing":"force",1);
    setenv("HYBRIS_BC_TRANSCODE",mode=="decode"?"0":"1",1);
    setenv("HYBRIS_BC_TRANSCODE_QUALITY",(mode=="fast"||mode=="high"||mode=="default")?mode.c_str():"auto",1);
    setenv("HYBRIS_VULKAN_TRACE","1",1);
    BcContext context;
    context.initialize(directory,compat);
    std::ostringstream log;
    log<<"GPU: "<<context.properties.deviceName<<"\nDriver: "<<context.properties.driverVersion
       <<"\nVulkan: "<<VK_VERSION_MAJOR(context.properties.apiVersion)<<"."<<VK_VERSION_MINOR(context.properties.apiVersion)
       <<"\nMode: "<<mode<<", image "<<size<<"x"<<size<<"\nNative BC: "<<context.native_bc<<", reported BC: "<<context.features.textureCompressionBC
       <<"\nChecks: sampled pixels against independent fixtures, exact compressed readback, compute-state restoration.\n";
    if(!context.features.textureCompressionBC) return log.str()+"UNSUPPORTED: BC unavailable on this route.\n";
    uint32_t failed=0,index=0;
    for(const Fixture &fixture:fixtures) {
        __android_log_print(ANDROID_LOG_INFO,"HybrisDiagnostics","BEGIN %s size=%u mode=%s",fixture.name,size,mode.c_str());
        try {
            Result result=sample(context,fixture,size);
            double error=0;uint32_t invalid=0;
            for(uint32_t y=0;y<sample_size;++y)for(uint32_t x=0;x<sample_size;++x)for(uint32_t c=0;c<4;++c) {
                float want=reference(fixture,(y%4)*4+(x%4),c);
                float got=result.sampled[(y*sample_size+x)*4+c];
                // ASTC HDR, like upstream, cannot represent negative values.
                if(compat && mode!="decode" && (mode!="normal" || !context.native_bc) && context.hdr_target && fixture.signed_hdr)
                    want=std::max(0.0f,want);
                if(!std::isfinite(got)){++invalid;continue;}
                double difference=(got-want)/std::max(1.0f,std::abs(want));error+=difference*difference;
            }
            double rms=std::sqrt(error/(sample_size*sample_size*4));
            bool pass=!invalid && result.raw_equal && rms<((compat && mode!="decode")?0.12:0.015);
            log<<(pass?"PASS ":"FAIL ")<<fixture.name<<" rms="<<rms<<" nonfinite="<<invalid
               <<" raw="<<(result.raw_equal?"exact":"MISMATCH")<<" ms="<<result.milliseconds<<"\n";
            if(!pass)++failed;
            for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x) {
                uint32_t p=((y/2)*sample_size+x/2)*4;
                auto color=[&](uint32_t c){float v=result.sampled[p+c];if(!std::isfinite(v))v=0;return static_cast<uint32_t>(std::clamp(v,0.0f,1.0f)*255+0.5f);};
                uint32_t at=(index/4*64+y)*256+(index%4*64+x);
                preview[at]=0xff000000u|(color(0)<<16)|(color(1)<<8)|color(2);
            }
        } catch(const std::exception &failure) {log<<"FAIL "<<fixture.name<<": "<<failure.what()<<"\n";++failed;if(!context.idle)break;}
        ++index;
    }
    log<<(failed?"FAIL: ":"PASS: ")<<index<<" cases, "<<failed<<" failed. Offscreen BC only; presentation and games require separate acceptance.\n";
    return log.str();
}
}

int main(int argc,char **argv) {
    if(argc!=5) {fprintf(stderr,"Expected LIBRARY_DIRECTORY MODE SIZE RESULT_DIRECTORY\n");return 2;}
    const std::string mode=argv[2];
    const std::array<std::string,7> modes={"native","normal","decode","auto","fast","default","high"};
    if(std::find(modes.begin(),modes.end(),mode)==modes.end() ||
       (std::string(argv[3])!="64" && std::string(argv[3])!="1024" && std::string(argv[3])!="4096"))return 2;
    const std::string files=argv[4],trace_name=files+"/trace.txt";
    std::string result;
    try {
        if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables environment-controlled diagnostics");
        result="AT_SECURE: 0\n"+run_probe(argv[1],mode,static_cast<uint32_t>(std::stoul(argv[3])));
        fflush(stderr);
        std::ifstream input(trace_name);
        std::string line;uint32_t emulated=0;
        while(std::getline(input,line)) {
            if(line.find("HYBRIS_BC_IMAGE ")==0)++emulated;
        }
        result+="Emulated image records: "+std::to_string(emulated)+"\n";
        if(mode!="native" && mode!="normal" && !emulated)
            result+="FAIL: forced BC path was not observed.\n";
        std::ofstream image(files+"/preview.argb",std::ios::binary);
        image.write(reinterpret_cast<const char *>(preview.data()),preview.size()*sizeof(preview[0]));
        if(!image)throw std::runtime_error("Cannot save preview pixels");
    }
    catch(const std::exception &failure){result="FAIL: "+std::string(failure.what())+"\n";}
    return report_result(result);
}
