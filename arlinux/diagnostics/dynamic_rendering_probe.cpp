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
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);Graphics graphics(c,rendering);
    std::ostringstream log;log<<"GPU: "<<c.properties.deviceName<<" dynamic="<<rendering<<" features2="<<features2<<"\n";
    unsigned cases=0,failed=0;
    for(int state=0;state<2;++state)for(int resolver=0;resolver<2;++resolver)for(int secondary=0;secondary<2;++secondary) {
        bool pass=graphics.render(output,state,resolver,secondary);++cases;if(!pass)++failed;
        log<<(pass?"PASS ":"FAIL ")<<"state="<<state<<" resolver="<<(resolver?"GIPA":"GDPA")<<" secondary="<<secondary<<" RGBA pixels and caller-owned input\n";
    }
    log<<(failed?"FAIL: ":"PASS: ")<<cases<<" actual GPU graphics cases, "<<failed<<" failed\n";return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="rendering-native"&&mode!="rendering-normal"&&mode!="rendering-force")return 2;
    try{result=run(argv[1],mode,false)+run(argv[1],mode,true);}catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
