/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "robust_null.inc"
#include "robust_buffer.inc"
#include "robust_image.inc"
using namespace diagnostics;
namespace {
struct RobustnessContext:Context {
    VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT native_robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceRobustness2PropertiesEXT robust_limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT};
    bool native_ext=false,native_khr=false,route_available=false;
    std::string requested_extension;
    void initialize(const std::string &directory,bool compat) {
        Context::initialize(directory,compat,VK_API_VERSION_1_3);
        auto enumerate_extensions=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(native_gipa(instance,"vkEnumerateDeviceExtensionProperties"));
        uint32_t extension_count=0;
        CHECK(enumerate_extensions(physical,nullptr,&extension_count,nullptr));
        std::vector<VkExtensionProperties> extension_properties(extension_count);
        CHECK(enumerate_extensions(physical,nullptr,&extension_count,extension_properties.data()));
        for(const auto &extension:extension_properties) {
            native_ext|=!strcmp(extension.extensionName,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
            native_khr|=!strcmp(extension.extensionName,VK_KHR_ROBUSTNESS_2_EXTENSION_NAME);
        }
        requested_extension=native_khr?VK_KHR_ROBUSTNESS_2_EXTENSION_NAME:VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
        auto exposed=instance_proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        CHECK(exposed(physical,nullptr,&extension_count,nullptr));
        extension_properties.resize(extension_count);
        CHECK(exposed(physical,nullptr,&extension_count,extension_properties.data()));
        for(const auto &extension:extension_properties)
            route_available|=requested_extension==extension.extensionName;
        if(native_ext || native_khr) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&native_robust};
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(native_gipa(instance,"vkGetPhysicalDeviceFeatures2"))(physical,&query);
        }
        if(route_available) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&robust};
            instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&query);
            VkPhysicalDeviceProperties2 limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&robust_limits};
            instance_proc<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical,&limits);
        }
        if(!route_available)return;
        VkPhysicalDeviceFeatures enabled{};enabled.robustBufferAccess=robust.robustBufferAccess2;
        const char *extension=requested_extension.c_str();
        create_device(enabled,reinterpret_cast<VkBaseOutStructure*>(&robust),1,&extension);
    }
};
// Device-side probes for native robustness and the compatibility approximation.
struct RobustDispatch {
    Context &c;
    ImageResources resources;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorSet set=VK_NULL_HANDLE;
    RobustDispatch(Context &context,const uint32_t *code,size_t bytes,const std::vector<VkDescriptorType> &types):c(context),resources(c) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorPoolSize> sizes;
        for(uint32_t i=0;i<types.size();++i) {
            bindings.push_back({i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
            sizes.push_back({types[i],2});
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount=bindings.size();info.pBindings=bindings.data();
        CHECK(c.CreateDescriptorSetLayout(c.device,&info,nullptr,&set_layout));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo layouts{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layouts.setLayoutCount=1;layouts.pSetLayouts=&set_layout;layouts.pushConstantRangeCount=1;layouts.pPushConstantRanges=&range;
        CHECK(c.CreatePipelineLayout(c.device,&layouts,nullptr,&layout));
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=bytes;shader_info.pCode=code;
        VkShaderModule module;CHECK(c.CreateShaderModule(c.device,&shader_info,nullptr,&module));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};compute.layout=layout;
        VkResult result=c.CreateComputePipelines(c.device,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline);
        c.DestroyShaderModule(c.device,module,nullptr);CHECK(result);
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;pool.maxSets=2;pool.poolSizeCount=sizes.size();pool.pPoolSizes=sizes.data();
        CHECK(c.CreateDescriptorPool(c.device,&pool,nullptr,&resources.descriptors));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=resources.descriptors;allocate.descriptorSetCount=1;allocate.pSetLayouts=&set_layout;
        CHECK(c.AllocateDescriptorSets(c.device,&allocate,&set));
    }
    ~RobustDispatch() {
        if(!c.idle)return;
        if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);
        if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);
        if(set_layout)c.DestroyDescriptorSetLayout(c.device,set_layout,nullptr);
    }
    VkWriteDescriptorSet write(uint32_t binding,VkDescriptorType type) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=binding;w.descriptorCount=1;w.descriptorType=type;return w;
    }
    void dispatch(Buffer &output,const uint32_t *push=nullptr,const std::function<void(VkCommandBuffer)> &prepare={}) {
        memset(output.mapped,0xcd,256);output.visibility(true);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool=c.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        CHECK(c.AllocateCommandBuffers(c.device,&allocate,&resources.command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(resources.command,&begin));
        if(prepare)prepare(resources.command);
        c.CmdBindPipeline(resources.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        c.CmdBindDescriptorSets(resources.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        if(push)c.CmdPushConstants(resources.command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,push);
        c.CmdDispatch(resources.command,1,1,1);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        c.CmdPipelineBarrier(resources.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(c.EndCommandBuffer(resources.command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&resources.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&resources.command;
        CHECK(c.QueueSubmit(c.queue,1,&submit,resources.fence));c.idle=false;
        CHECK(c.WaitForFences(c.device,1,&resources.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);
    }
};
std::string run_robust(const std::string &directory,const std::string &mode) {
    setenv("HYBRIS_BC_TEXTURES","0",1);
    setenv("HYBRIS_ROBUSTNESS2",mode=="robust-force"?"force":mode=="robust-off"?"0":"auto",1);
    setenv("HYBRIS_VULKAN_TRACE","1",1);
    RobustnessContext c;c.initialize(directory,mode!="robust-native");
    std::ostringstream log;
    log<<"GPU: "<<c.properties.deviceName<<"\nMode: "<<mode<<"\nNative robustness2: buffer="<<c.native_robust.robustBufferAccess2
       <<" image="<<c.native_robust.robustImageAccess2<<" null="<<c.native_robust.nullDescriptor<<"\nReported: buffer="<<c.robust.robustBufferAccess2
       <<" image="<<c.robust.robustImageAccess2<<" null="<<c.robust.nullDescriptor<<"\n";
    if(!c.route_available)return log.str()+"UNSUPPORTED: robustness2 extension unavailable on this route.\n";
    bool force=mode=="robust-force";
    bool compat_null=force || !c.native_robust.nullDescriptor;
    bool compat_buffer=force || !c.native_robust.robustBufferAccess2;
    bool compat_image=force || !c.native_robust.robustImageAccess2;
    unsigned failed=0,checks=0,limits=0;
    auto values=[&](const char *name,Buffer &output,uint32_t count,const std::vector<uint32_t> &required,bool approximate) {
        const uint32_t *v=static_cast<const uint32_t *>(output.mapped);
        bool exact=true,pass=true;
        log<<name<<":";
        for(uint32_t i=0;i<count;++i){log<<" 0x"<<std::hex<<v[i]<<std::dec;exact&=v[i]==0;}
        for(uint32_t i:required)pass&=v[i]==0;
        if(!approximate)pass&=exact;
        if(approximate && !exact){++limits;log<<" [COMPATIBILITY LIMIT: writable/OOB reads differ from strict zero semantics]";}
        log<<(pass?" PASS\n":" FAIL\n");++checks;if(!pass)++failed;
    };
    Buffer output(c,256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if(c.robust.nullDescriptor) {
        std::vector<VkDescriptorType> types={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        RobustDispatch dispatch(c,robust_null_spv,sizeof(robust_null_spv),types);
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        CHECK(c.CreateSampler(c.device,&sampler,nullptr,&dispatch.resources.sampler));
        VkDescriptorBufferInfo buffers[]={{output.handle,0,256},{VK_NULL_HANDLE,0,VK_WHOLE_SIZE},{VK_NULL_HANDLE,0,VK_WHOLE_SIZE}};
        VkBufferView views[2]={};
        VkDescriptorImageInfo images[]={{dispatch.resources.sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL}};
        std::vector<VkWriteDescriptorSet> writes;
        for(uint32_t i=0;i<types.size();++i) {
            auto w=dispatch.write(i,types[i]);
            if(i<3)w.pBufferInfo=&buffers[i];else if(i<5)w.pTexelBufferView=&views[i-3];else w.pImageInfo=&images[i-5];
            writes.push_back(w);
        }
        c.UpdateDescriptorSets(c.device,writes.size(),writes.data(),0,nullptr);
        if(buffers[1].buffer || buffers[2].buffer || views[0] || views[1] || images[0].imageView || images[1].imageView)
            throw std::runtime_error("Layer modified caller-owned descriptor writes");
        dispatch.dispatch(output);
        values("Null writes",output,14,{0,1,5,9,10},compat_null);
        // The second round uses immutable template data and the same real GPU path.
        struct Data {VkDescriptorBufferInfo b[3];VkBufferView v[2];VkDescriptorImageInfo i[2];} data{};
        memcpy(data.b,buffers,sizeof(buffers));memcpy(data.v,views,sizeof(views));memcpy(data.i,images,sizeof(images));
        Data original=data;
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
        for(uint32_t i=0;i<types.size();++i) {
            size_t offset=i<3?offsetof(Data,b)+i*sizeof(buffers[0]):i<5?offsetof(Data,v)+(i-3)*sizeof(views[0]):offsetof(Data,i)+(i-5)*sizeof(images[0]);
            entries.push_back({i,0,1,types[i],offset,0});
        }
        VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        info.descriptorUpdateEntryCount=entries.size();info.pDescriptorUpdateEntries=entries.data();info.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;info.descriptorSetLayout=dispatch.set_layout;
        VkDescriptorUpdateTemplate tmpl;CHECK(c.CreateDescriptorUpdateTemplate(c.device,&info,nullptr,&tmpl));
        c.UpdateDescriptorSetWithTemplate(c.device,dispatch.set,tmpl,&data);c.DestroyDescriptorUpdateTemplate(c.device,tmpl,nullptr);
        if(memcmp(&original,&data,sizeof(data)))throw std::runtime_error("Layer modified caller-owned template data");
        c.FreeCommandBuffers(c.device,c.pool,1,&dispatch.resources.command);dispatch.resources.command=VK_NULL_HANDLE;
        c.DestroyFence(c.device,dispatch.resources.fence,nullptr);dispatch.resources.fence=VK_NULL_HANDLE;
        dispatch.dispatch(output);values("Null template",output,14,{0,5,9},compat_null);
        VkDescriptorSet old=dispatch.set;
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=dispatch.resources.descriptors;allocate.descriptorSetCount=1;allocate.pSetLayouts=&dispatch.set_layout;
        CHECK(c.AllocateDescriptorSets(c.device,&allocate,&dispatch.set));
        std::vector<VkCopyDescriptorSet> copies;
        for(uint32_t i=0;i<types.size();++i) {
            VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};copy.srcSet=old;copy.dstSet=dispatch.set;copy.srcBinding=copy.dstBinding=i;copy.descriptorCount=1;copies.push_back(copy);
        }
        c.UpdateDescriptorSets(c.device,0,nullptr,copies.size(),copies.data());
        CHECK(c.FreeDescriptorSets(c.device,dispatch.resources.descriptors,1,&old));
        c.FreeCommandBuffers(c.device,c.pool,1,&dispatch.resources.command);dispatch.resources.command=VK_NULL_HANDLE;
        c.DestroyFence(c.device,dispatch.resources.fence,nullptr);dispatch.resources.fence=VK_NULL_HANDLE;
        dispatch.dispatch(output);values("Null descriptor copy",output,14,{0,5,9},compat_null);
        CHECK(c.ResetDescriptorPool(c.device,dispatch.resources.descriptors,0));

    }
    if(c.robust.robustBufferAccess2) {
        Buffer storage(c,512,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),uniform(c,512,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        std::fill_n(static_cast<uint32_t *>(storage.mapped),128,0x31323334u);std::fill_n(static_cast<uint32_t *>(uniform.mapped),128,0x31323334u);
        storage.visibility(true);uniform.visibility(true);
        RobustDispatch dispatch(c,robust_buffer_spv,sizeof(robust_buffer_spv),{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER});
        VkDeviceSize sr=std::max<VkDeviceSize>(4,c.robust_limits.robustStorageBufferAccessSizeAlignment),ur=std::max<VkDeviceSize>(16,c.robust_limits.robustUniformBufferAccessSizeAlignment);
        if(sr>256 || ur>256)throw std::runtime_error("Invalid robustness2 alignment");
        VkDescriptorBufferInfo buffers[]={{output.handle,0,256},{storage.handle,0,sr},{uniform.handle,0,ur}};
        VkWriteDescriptorSet writes[3];for(uint32_t i=0;i<3;++i){writes[i]=dispatch.write(i,i==2?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);writes[i].pBufferInfo=&buffers[i];}
        c.UpdateDescriptorSets(c.device,3,writes,0,nullptr);
        uint32_t push[]={static_cast<uint32_t>(sr/4),static_cast<uint32_t>(ur/16)};dispatch.dispatch(output,push);
        values("Buffer OOB",output,4,{},compat_buffer);
        storage.visibility(false);bool unchanged=std::all_of(static_cast<uint32_t *>(storage.mapped),static_cast<uint32_t *>(storage.mapped)+128,[](uint32_t v){return v==0x31323334u;});
        if(!unchanged){log<<(compat_buffer?"COMPATIBILITY LIMIT":"FAIL")<<": OOB store modified allocated buffer\n";if(compat_buffer)++limits;else ++failed;}
    }
    if(c.robust.robustImageAccess2) {
        RobustDispatch dispatch(c,robust_image_spv,sizeof(robust_image_spv),{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        auto &r=dispatch.resources;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R32_UINT;image.extent={1,1,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
        image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(c.CreateImage(c.device,&image,nullptr,&r.image));VkMemoryRequirements need;c.GetImageMemoryRequirements(c.device,r.image,&need);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=need.size;alloc.memoryTypeIndex=c.memory_type(need.memoryTypeBits,0);
        CHECK(c.AllocateMemory(c.device,&alloc,nullptr,&r.memory));CHECK(c.BindImageMemory(c.device,r.image,r.memory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=r.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=VK_FORMAT_R32_UINT;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        CHECK(c.CreateImageView(c.device,&view,nullptr,&r.view));
        VkDescriptorBufferInfo b{output.handle,0,256};VkDescriptorImageInfo i{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};
        auto w0=dispatch.write(0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),w1=dispatch.write(1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);w0.pBufferInfo=&b;w1.pImageInfo=&i;
        VkWriteDescriptorSet writes[]={w0,w1};c.UpdateDescriptorSets(c.device,2,writes,0,nullptr);
        dispatch.dispatch(output,nullptr,[&](VkCommandBuffer command){
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=r.image;barrier.subresourceRange=view.subresourceRange;
            c.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkClearColorValue color{};color.uint32[0]=0x31323334u;c.CmdClearColorImage(command,r.image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&view.subresourceRange);
            barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            c.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        });
        values("Image OOB",output,3,{},compat_image);
        if(static_cast<uint32_t *>(output.mapped)[3]!=0x31323334u){log<<"FAIL: valid image pixel corrupted\n";++failed;}
    }
    if(!checks)return log.str()+"UNSUPPORTED: no robustness2 feature is exposed.\n";
    log<<(failed?"FAIL: ":"PASS: ")<<checks<<" GPU checks, "<<failed<<" failures, "<<limits<<" compatibility limitations. This is not a Vulkan conformance result.\n";
    return log.str();
}

} // namespace
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"Expected LIBRARY_DIRECTORY MODE RESULT_DIRECTORY\n");return 2;}
    const std::string mode=argv[2];
    const std::array<std::string,4> modes={"robust-native","robust","robust-force","robust-off"};
    if(std::find(modes.begin(),modes.end(),mode)==modes.end())return 2;
    std::string result;
    try {
        if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables environment-controlled diagnostics");
        result="AT_SECURE: 0\n"+run_robust(argv[1],mode);
        if(mode=="robust-force") {
            fflush(stderr);
            std::ifstream trace(std::string(argv[3])+"/trace.txt");
            std::string line;bool observed=false;
            while(std::getline(trace,line))if(line.find("HYBRIS_ROBUSTNESS2 compatibility=")==0)observed=true;
            if(!observed)result+="FAIL: forced robustness2 compatibility was not observed.\n";
        }
    } catch(const std::exception &failure){result="FAIL: "+std::string(failure.what())+"\n";}
    return report_result(result);
}
