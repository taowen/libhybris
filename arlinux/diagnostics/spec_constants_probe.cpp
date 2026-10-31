/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"
#include "spec_constants.inc"
using namespace diagnostics;
namespace {
struct SpecResources {
    Context &c;
    ImageResources image;
    VkDescriptorSetLayout sets=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkShaderModule shader=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    bool uploaded=false;
    SpecResources(Context &context,Buffer &output):c(context),image(c) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType=VK_IMAGE_TYPE_2D;create.format=VK_FORMAT_R32G32B32A32_SFLOAT;create.extent={1,1,1};
        create.mipLevels=create.arrayLayers=1;create.samples=VK_SAMPLE_COUNT_1_BIT;create.tiling=VK_IMAGE_TILING_OPTIMAL;
        create.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        CHECK(c.CreateImage(c.device,&create,nullptr,&image.image));
        VkMemoryRequirements need;c.GetImageMemoryRequirements(c.device,image.image,&need);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=need.size;allocation.memoryTypeIndex=c.memory_type(need.memoryTypeBits,0);
        CHECK(c.AllocateMemory(c.device,&allocation,nullptr,&image.memory));CHECK(c.BindImageMemory(c.device,image.image,image.memory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=create.format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        CHECK(c.CreateImageView(c.device,&view,nullptr,&image.view));
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        CHECK(c.CreateSampler(c.device,&sampler,nullptr,&image.sampler));
        VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set_info.bindingCount=2;set_info.pBindings=bindings;
        CHECK(c.CreateDescriptorSetLayout(c.device,&set_info,nullptr,&sets));
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout_info.setLayoutCount=1;layout_info.pSetLayouts=&sets;
        CHECK(c.CreatePipelineLayout(c.device,&layout_info,nullptr,&layout));
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
        CHECK(c.CreateDescriptorPool(c.device,&pool,nullptr,&image.descriptors));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=image.descriptors;allocate.descriptorSetCount=1;allocate.pSetLayouts=&sets;
        CHECK(c.AllocateDescriptorSets(c.device,&allocate,&set));
        VkDescriptorImageInfo texture{image.sampler,image.view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo buffer{output.handle,0,256};
        VkWriteDescriptorSet writes[2]{};
        for(uint32_t i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;}
        writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[0].pImageInfo=&texture;
        writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&buffer;c.UpdateDescriptorSets(c.device,2,writes,0,nullptr);
    }
    ~SpecResources() {
        if(!c.idle)return;
        if(pipeline)c.DestroyPipeline(c.device,pipeline,nullptr);
        if(shader)c.DestroyShaderModule(c.device,shader,nullptr);
        if(layout)c.DestroyPipelineLayout(c.device,layout,nullptr);
        if(sets)c.DestroyDescriptorSetLayout(c.device,sets,nullptr);
    }
    void dispatch(Buffer &input,Buffer &output,const uint32_t *code,size_t bytes,const VkSpecializationInfo *specialization,bool inline_module) {
        if(pipeline){c.DestroyPipeline(c.device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=bytes;module.pCode=code;
        if(!inline_module && !shader)CHECK(c.CreateShaderModule(c.device,&module,nullptr,&shader));
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.layout=layout;create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",specialization};
        if(inline_module){create.stage.module=VK_NULL_HANDLE;create.stage.pNext=&module;}
        CHECK(c.CreateComputePipelines(c.device,VK_NULL_HANDLE,1,&create,nullptr,&pipeline));
        if(image.command){c.FreeCommandBuffers(c.device,c.pool,1,&image.command);image.command=VK_NULL_HANDLE;}
        if(image.fence){c.DestroyFence(c.device,image.fence,nullptr);image.fence=VK_NULL_HANDLE;}
        memset(output.mapped,0xcd,256);output.visibility(true);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=c.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        CHECK(c.AllocateCommandBuffers(c.device,&allocate,&image.command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(image.command,&begin));
        if(!uploaded) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
            c.CmdCopyBufferToImage(image.command,input.handle,image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);uploaded=true;
        }
        c.CmdBindPipeline(image.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);c.CmdBindDescriptorSets(image.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);c.CmdDispatch(image.command,1,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        c.CmdPipelineBarrier(image.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);CHECK(c.EndCommandBuffer(image.command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(c.CreateFence(c.device,&fence,nullptr,&image.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&image.command;
        CHECK(c.QueueSubmit(c.queue,1,&submit,image.fence));c.idle=false;CHECK(c.WaitForFences(c.device,1,&image.fence,VK_TRUE,UINT64_C(10000000000)));c.idle=true;output.visibility(false);
    }
};
std::string run(const std::string &directory,const std::string &mode) {
    setenv("HYBRIS_BC_TEXTURES","0",1);setenv("HYBRIS_VULKAN_TRACE","1",1);
    setenv("HYBRIS_MALI_SPEC_CONSTANTS",mode=="spec-force"?"force":mode=="spec-off"?"0":"auto",1);
    Context c;c.initialize(directory,mode!="spec-native",VK_API_VERSION_1_3);
    uint32_t count=0;auto enumerate=c.instance_proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    CHECK(enumerate(c.physical,nullptr,&count,nullptr));std::vector<VkExtensionProperties> extensions(count);
    CHECK(enumerate(c.physical,nullptr,&count,extensions.data()));
    bool have_maintenance5=false;
    for(const auto &extension:extensions)have_maintenance5|=!strcmp(extension.extensionName,VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    if(have_maintenance5) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&maintenance};
        c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical,&query);
    }
    bool inline_available=have_maintenance5 && maintenance.maintenance5 && c.properties.apiVersion>=VK_API_VERSION_1_3;
    VkPhysicalDeviceFeatures enabled{};const char *extension=VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    c.create_device(enabled,inline_available?reinterpret_cast<VkBaseOutStructure*>(&maintenance):nullptr,inline_available?1:0,inline_available?&extension:nullptr);
    bool repair=mode=="spec-force" || (mode=="spec-normal" && c.properties.vendorID==0x13b5);
    Buffer input(c,16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT),output(c,256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const float pixel[]={0.25f,0.5f,0.75f,1.f};memcpy(input.mapped,pixel,sizeof(pixel));input.visibility(true);
    uint32_t pixel_bits[4];memcpy(pixel_bits,pixel,sizeof(pixel));
    std::ostringstream log;log<<"GPU: "<<c.properties.deviceName<<"\nMode: "<<mode<<"\nReference SPIR-V validated at build time. Historical application form used only with repair enabled.\n";
    unsigned failed=0,checks=0;
    for(int form=0;form<(repair?2:1);++form) {
        const uint32_t *code=form?spec_application:spec_reference;size_t bytes=form?sizeof(spec_application):sizeof(spec_reference);
        std::vector<uint32_t> before(code,code+bytes/4);
        SpecResources resources(c,output);
        // Reuse one module with different pipeline specializations; include default and partial overrides.
        for(unsigned variant=0;variant<6;++variant) {
            VkBool32 values[]={variant==4?1u:(variant&1),variant==5?1u:((variant>>1)&1)};
            VkSpecializationMapEntry entries[]={{0,0,4},{1,4,4}};
            VkSpecializationInfo specialization{variant==4?1u:2u,entries,sizeof(values),values};
            bool first=variant==5?false:values[0],second=variant>=4?true:values[1];
            resources.dispatch(input,output,code,bytes,variant==5?nullptr:&specialization,false);
            bool mask[3][4]={{first,second,first,true},{first,!first,second,false},{true,false,true,false}};
            bool pass=true;auto got=static_cast<uint32_t*>(output.mapped);
            for(unsigned group=0;group<3;++group)for(unsigned channel=0;channel<4;++channel)
                pass&=got[group*4+channel]==(mask[group][channel]?pixel_bits[channel]:0);
            pass&=memcmp(code,before.data(),bytes)==0;
            log<<(pass?"PASS ":"FAIL ")<<(form?"repaired":"reference")<<" specialization="<<variant<<" exact sampled pixels; caller code unchanged\n";
            if(!pass) {
                log<<"  got:";
                for(unsigned i=0;i<12;++i)log<<" "<<std::hex<<got[i];
                log<<"\n  want:";
                for(unsigned group=0;group<3;++group)for(unsigned channel=0;channel<4;++channel)log<<" "<<std::hex<<(mask[group][channel]?pixel_bits[channel]:0);
                log<<std::dec<<"\n";
            }
            ++checks;if(!pass)++failed;
        }
        // Only the active compatibility layer consumes historical inline modules.
        // Other routes never send this form to the raw Vulkan driver.
        if(form && inline_available) {
            VkBool32 values[]={1,0};VkSpecializationMapEntry entries[]={{0,0,4},{1,4,4}};
            VkSpecializationInfo specialization{2,entries,sizeof(values),values};
            resources.dispatch(input,output,code,bytes,&specialization,true);
            bool mask[3][4]={{true,false,true,true},{true,false,false,false},{true,false,true,false}};
            bool pass=memcmp(code,before.data(),bytes)==0;auto got=static_cast<uint32_t*>(output.mapped);
            for(unsigned group=0;group<3;++group)for(unsigned channel=0;channel<4;++channel)pass&=got[group*4+channel]==(mask[group][channel]?pixel_bits[channel]:0);
            log<<(pass?"PASS ":"FAIL ")<<"inline compute module, exact sampled pixels; caller code unchanged\n";++checks;if(!pass)++failed;
        }
    }
    if(repair && !inline_available)log<<"SKIP: native maintenance5 unavailable; inline compute not exercised.\n";
    if(!repair)log<<"Repair not selected: only valid native reference shaders executed.\n";
    log<<(failed?"FAIL: ":"PASS: ")<<checks<<" GPU cases, "<<failed<<" failed. No game-level acceptance claimed.\n";
    return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    const std::string mode=argv[2];const std::array<std::string,4> modes={"spec-native","spec-normal","spec-force","spec-off"};
    if(std::find(modes.begin(),modes.end(),mode)==modes.end())return 2;
    std::string result;
    try {
        if(getauxval(AT_SECURE))throw std::runtime_error("Secure execution disables environment-controlled diagnostics");
        result="AT_SECURE: 0\n"+run(argv[1],mode);
        fflush(stderr);std::ifstream trace(std::string(argv[3])+"/trace.txt");std::string line;bool observed=false;
        while(std::getline(trace,line))if(line.find("HYBRIS_MALI_SPEC_CONSTANTS first=")==0)observed=true;
        if(mode=="spec-force" && !observed)result+="FAIL: forced repair was not observed.\n";
        if((mode=="spec-off" || mode=="spec-native") && observed)result+="FAIL: disabled repair modified a module.\n";
    } catch(const std::exception &failure){result="FAIL: "+std::string(failure.what())+"\n";}
    return report_result(result);
}
