/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rendering_fixture.h"
using namespace rendering_diagnostics;
namespace {
std::string run(const std::string &directory,const std::string &mode,bool features2) {
    bool compat=mode!="maintenance-native";
    if(mode=="maintenance-force")setenv("HYBRIS_MAINTENANCE5","force",1);
    else unsetenv("HYBRIS_MAINTENANCE5");
    Context c;c.initialize(directory,compat);
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceMaintenance5Features maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&dynamic;dynamic.pNext=&maintenance;
    c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical,&query);
    bool rendering=dynamic.dynamicRendering;
    if(compat&&(!rendering || !maintenance.maintenance5))throw std::runtime_error("Maintenance5 or dynamic rendering was not exposed");
    bool enabled_maintenance=maintenance.maintenance5;
    const char *extensions[]={VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    if(!enabled_maintenance)dynamic.pNext=nullptr;
    uint32_t extension_count=rendering?(enabled_maintenance?2:1):0;
    VkPhysicalDeviceFeatures enabled{};
    VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};requested.features=enabled;requested.pNext=rendering?&dynamic:nullptr;
    if(features2)c.create_device(enabled,(VkBaseOutStructure*)&requested,extension_count,extension_count?extensions:nullptr,true);
    else c.create_device(enabled,rendering?(VkBaseOutStructure*)&dynamic:nullptr,extension_count,extension_count?extensions:nullptr);
    Buffer output(c,W*H*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);Graphics graphics(c,rendering,enabled_maintenance);
    std::ostringstream log;log<<"GPU: "<<c.properties.deviceName<<" maintenance="<<enabled_maintenance<<" dynamic="<<rendering<<" features2="<<features2<<"\n";
    log<<run_cases(graphics,output);return log.str();
}
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;std::string mode=argv[2],result;
    if(mode!="maintenance-native"&&mode!="maintenance-normal"&&mode!="maintenance-force")return 2;
    try{result=run(argv[1],mode,false)+run(argv[1],mode,true);}catch(const std::exception &e){result="FAIL: "+std::string(e.what())+"\n";}
    return report_result(result);
}
