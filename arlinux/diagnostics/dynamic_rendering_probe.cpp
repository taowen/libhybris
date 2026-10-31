/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rendering_fixture.h"
using namespace rendering_diagnostics;
namespace {
std::string run(const std::string &directory,const std::string &mode,bool features2) {
    bool compat=mode!="rendering-native";
    if(mode=="rendering-force")setenv("HYBRIS_DYNAMIC_RENDERING","force",1);
    else unsetenv("HYBRIS_DYNAMIC_RENDERING");
    Context c;c.initialize(directory,compat);
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&dynamic;
    c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical,&query);
    bool rendering=dynamic.dynamicRendering;
    if(compat&&!rendering)throw std::runtime_error("Dynamic rendering was not exposed");
    const char *extension=VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    VkPhysicalDeviceFeatures enabled{};
    VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};requested.features=enabled;requested.pNext=rendering?&dynamic:nullptr;
    if(features2)c.create_device(enabled,(VkBaseOutStructure*)&requested,rendering?1:0,rendering?&extension:nullptr,true);
    else c.create_device(enabled,rendering?(VkBaseOutStructure*)&dynamic:nullptr,rendering?1:0,rendering?&extension:nullptr);
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);Graphics graphics(c,rendering,false,compat);
    std::ostringstream log;log<<"GPU: "<<c.properties.deviceName<<" dynamic="<<rendering<<" features2="<<features2<<"\n";
    log<<run_cases(graphics,output);
    bool native_bind=c.native_gdpa(c.device,"vkCmdBindVertexBuffers2") ||
                     c.native_gdpa(c.device,"vkCmdBindVertexBuffers2EXT");
    log<<"Vertex buffers2 native="<<native_bind<<" static GPU fetch="<<compat<<"\n";
    if(compat && !native_bind) {
        Buffer vertices(c,64,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        for(const char *name:{"vkCmdBindVertexBuffers2","vkCmdBindVertexBuffers2EXT"})for(int stride=0;stride<2;++stride) {
            ImageResources recording(c);
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=c.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
            CHECK(c.AllocateCommandBuffers(c.device,&allocate,&recording.command));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(c.BeginCommandBuffer(recording.command,&begin));
            auto bind=(PFN_vkCmdBindVertexBuffers2)c.gdpa(c.device,name);
            if(!bind)throw std::runtime_error(std::string("Missing ")+name);
            VkDeviceSize offset=0,size=64,step=8;
            bind(recording.command,0,1,&vertices.handle,&offset,stride?nullptr:&size,stride?&step:nullptr);
            VkResult result=c.EndCommandBuffer(recording.command);
            if(result!=VK_ERROR_FEATURE_NOT_PRESENT)throw std::runtime_error("Unsupported vertex parameters were not rejected");
            log<<"PASS "<<name<<" rejects "<<(stride?"dynamic stride":"explicit size")<<" before submit\n";
        }
        log<<run_cases(graphics,output);
    }
    return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="rendering-native"&&mode!="rendering-normal"&&mode!="rendering-force")return 2;
    try{result=run(argv[1],mode,false)+run(argv[1],mode,true);}catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
