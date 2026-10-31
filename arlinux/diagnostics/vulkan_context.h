/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <dlfcn.h>
#include <array>
#include <vector>
#include <string>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <android/log.h>
#include <functional>

namespace diagnostics {
inline void check(VkResult result,const char *operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult="+std::to_string(result));
}
#define CHECK(call) check((call),#call)
#define DEVICE_PROCS(V) \
 V(DestroyDevice) V(GetDeviceQueue) V(CreateBuffer) V(DestroyBuffer) V(GetBufferMemoryRequirements) \
 V(AllocateMemory) V(FreeMemory) V(BindBufferMemory) V(MapMemory) V(UnmapMemory) \
 V(FlushMappedMemoryRanges) V(InvalidateMappedMemoryRanges) V(CreateImage) V(DestroyImage) \
 V(GetImageMemoryRequirements) V(BindImageMemory) V(CreateImageView) V(DestroyImageView) \
 V(CreateSampler) V(DestroySampler) V(CreateShaderModule) V(DestroyShaderModule) \
 V(CreateDescriptorSetLayout) V(DestroyDescriptorSetLayout) V(CreatePipelineLayout) V(DestroyPipelineLayout) \
 V(CreateComputePipelines) V(DestroyPipeline) V(CreateDescriptorPool) V(DestroyDescriptorPool) \
 V(AllocateDescriptorSets) V(FreeDescriptorSets) V(ResetDescriptorPool) V(UpdateDescriptorSets) V(CreateCommandPool) V(DestroyCommandPool) \
 V(AllocateCommandBuffers) V(FreeCommandBuffers) V(BeginCommandBuffer) V(EndCommandBuffer) \
 V(CmdPipelineBarrier) V(CmdCopyBufferToImage) V(CmdCopyImageToBuffer) V(CmdBindPipeline) \
 V(CmdBindDescriptorSets) V(CmdPushConstants) V(CmdDispatch) V(CreateFence) V(DestroyFence) \
 V(QueueSubmit) V(WaitForFences) V(CmdClearColorImage) \
 V(CreateDescriptorUpdateTemplate) V(DestroyDescriptorUpdateTemplate) V(UpdateDescriptorSetWithTemplate)

struct Context {
    VkInstance instance=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    uint32_t family=0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool=VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr native_gipa=nullptr,gipa=nullptr;
    PFN_vkGetDeviceProcAddr native_gdpa=nullptr,gdpa=nullptr;
    PFN_vkDestroyInstance destroy_instance=nullptr;
    bool layered=false, idle=true;
    VkPhysicalDeviceFeatures original{};
    PFN_vkGetDeviceProcAddr layer_gdpa=nullptr;
#define DECLARE(name) PFN_vk##name name=nullptr;
    DEVICE_PROCS(DECLARE)
#undef DECLARE

    template<class T> T instance_proc(const char *name) {
        auto result=reinterpret_cast<T>(gipa(instance,name));
        if(!result) throw std::runtime_error(std::string("Missing ")+name);
        return result;
    }
    void initialize(const std::string &directory,bool compat,uint32_t requested_api=VK_API_VERSION_1_1) {
        layered=compat;
        void *system=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
        if(!system) throw std::runtime_error(dlerror());
        native_gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(system,"vkGetInstanceProcAddr"));
        if(!native_gipa) throw std::runtime_error("System Vulkan resolver missing");
        gipa=native_gipa;
        VkNegotiateLayerInterface layer{};
        if(compat) {
            void *library=dlopen((directory+"/libVkLayer_hybris_compat.so").c_str(),RTLD_NOW|RTLD_LOCAL);
            if(!library) throw std::runtime_error(dlerror());
            auto negotiate=reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(dlsym(library,"vkNegotiateLoaderLayerInterfaceVersion"));
            if(!negotiate) throw std::runtime_error("Compatibility layer entry missing");
            layer.sType=LAYER_NEGOTIATE_INTERFACE_STRUCT;
            layer.loaderLayerInterfaceVersion=2;
            CHECK(negotiate(&layer));
            gipa=layer.pfnGetInstanceProcAddr;
            layer_gdpa=layer.pfnGetDeviceProcAddr;
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="libhybris-diagnostics";
        app.apiVersion=VK_API_VERSION_1_1;
        if(requested_api>VK_API_VERSION_1_1) {
            auto version=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(native_gipa(nullptr,"vkEnumerateInstanceVersion"));
            uint32_t supported=VK_API_VERSION_1_1;
            if(version)CHECK(version(&supported));
            app.apiVersion=std::min(supported,requested_api);
        }
        VkLayerInstanceLink next{};
        next.pfnNextGetInstanceProcAddr=native_gipa;
        next.pfnNextGetPhysicalDeviceProcAddr=reinterpret_cast<PFN_GetPhysicalDeviceProcAddr>(native_gipa);
        VkLayerInstanceCreateInfo link{};
        link.sType=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
        link.function=VK_LAYER_LINK_INFO;
        link.u.pLayerInfo=&next;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo=&app;
        create.pNext=compat?&link:nullptr;
        auto create_instance=instance_proc<PFN_vkCreateInstance>("vkCreateInstance");
        CHECK(create_instance(&create,nullptr,&instance));
        destroy_instance=instance_proc<PFN_vkDestroyInstance>("vkDestroyInstance");
        uint32_t count=1;
        auto enumerate=instance_proc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        VkResult result=enumerate(instance,&count,&physical);
        if((result!=VK_SUCCESS && result!=VK_INCOMPLETE)||!count) throw std::runtime_error("No physical Vulkan device");
        instance_proc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&properties);
        instance_proc<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(physical,&features);
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(native_gipa(instance,"vkGetPhysicalDeviceFeatures"))(physical,&original);
        instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,&memory);
        auto query_queues=instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        query_queues(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        query_queues(physical,&count,queues.data());
        while(family<count && !(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family;
        if(family==count) throw std::runtime_error("No compute queue");
    }
    void create_device(const VkPhysicalDeviceFeatures &enabled,VkBaseOutStructure *extended=nullptr,
                       uint32_t extension_count=0,const char *const *extensions=nullptr) {
        native_gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(native_gipa(instance,"vkGetDeviceProcAddr"));
        gdpa=layered?layer_gdpa:native_gdpa;
        VkLayerDeviceLink device_next{};
        device_next.pfnNextGetInstanceProcAddr=native_gipa;
        device_next.pfnNextGetDeviceProcAddr=native_gdpa;
        VkLayerDeviceCreateInfo device_link{};
        device_link.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        device_link.function=VK_LAYER_LINK_INFO;
        device_link.u.pLayerInfo=&device_next;
        float priority=1;
        VkDeviceQueueCreateInfo queue_create{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex=family;
        queue_create.queueCount=1;
        queue_create.pQueuePriorities=&priority;
        VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        VkBaseOutStructure *tail=extended;
        while(tail && tail->pNext)tail=tail->pNext;
        if(tail)tail->pNext=layered?reinterpret_cast<VkBaseOutStructure*>(&device_link):nullptr;
        device_create.pNext=extended?extended:(layered?static_cast<void*>(&device_link):nullptr);
        device_create.enabledExtensionCount=extension_count;
        device_create.ppEnabledExtensionNames=extensions;
        device_create.pEnabledFeatures=&enabled;
        device_create.queueCreateInfoCount=1;
        device_create.pQueueCreateInfos=&queue_create;
        auto create_device=instance_proc<PFN_vkCreateDevice>("vkCreateDevice");
        VkResult result=create_device(physical,&device_create,nullptr,&device);
        if(tail)tail->pNext=nullptr;
        CHECK(result);
#define LOAD(name) name=reinterpret_cast<PFN_vk##name>(gdpa(device,"vk" #name)); if(!name) throw std::runtime_error("Missing vk" #name);
        DEVICE_PROCS(LOAD)
#undef LOAD
        GetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commands.queueFamilyIndex=family;
        CHECK(CreateCommandPool(device,&commands,nullptr,&pool));
    }
    uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags required) {
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&required)==required) return i;
        throw std::runtime_error("No compatible memory type");
    }
    ~Context() {
        if(!idle)return; // A timed-out submission is retired by process exit.
        if(device && DestroyDevice) {
            if(pool) DestroyCommandPool(device,pool,nullptr);
            DestroyDevice(device,nullptr);
        }
        if(instance && destroy_instance) destroy_instance(instance,nullptr);
    }
};

struct Buffer {
    Context &c;
    VkBuffer handle=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    void *mapped=nullptr;
    Buffer(Context &context,VkDeviceSize bytes,VkBufferUsageFlags usage):c(context) {
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size=bytes;create.usage=usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        CHECK(c.CreateBuffer(c.device,&create,nullptr,&handle));
        VkMemoryRequirements need;c.GetBufferMemoryRequirements(c.device,handle,&need);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=need.size;
        allocation.memoryTypeIndex=c.memory_type(need.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        CHECK(c.AllocateMemory(c.device,&allocation,nullptr,&memory));
        CHECK(c.BindBufferMemory(c.device,handle,memory,0));
        CHECK(c.MapMemory(c.device,memory,0,VK_WHOLE_SIZE,0,&mapped));
    }
    void visibility(bool flush) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=memory;range.size=VK_WHOLE_SIZE;
        if(flush) CHECK(c.FlushMappedMemoryRanges(c.device,1,&range));
        else CHECK(c.InvalidateMappedMemoryRanges(c.device,1,&range));
    }
    ~Buffer() {
        if(!c.idle)return;
        if(mapped) c.UnmapMemory(c.device,memory);
        if(handle)c.DestroyBuffer(c.device,handle,nullptr);
        if(memory)c.FreeMemory(c.device,memory,nullptr);
    }
};
struct ImageResources {
    Context &c;
    VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;VkSampler sampler=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    explicit ImageResources(Context &context):c(context){}
    ~ImageResources() {
        if(!c.idle)return;
        if(fence)c.DestroyFence(c.device,fence,nullptr);
        if(command)c.FreeCommandBuffers(c.device,c.pool,1,&command);
        if(descriptors)c.DestroyDescriptorPool(c.device,descriptors,nullptr);
        if(sampler)c.DestroySampler(c.device,sampler,nullptr);
        if(view)c.DestroyImageView(c.device,view,nullptr);
        if(image)c.DestroyImage(c.device,image,nullptr);
        if(memory)c.FreeMemory(c.device,memory,nullptr);
    }
};

inline int report_result(const std::string &result) {
    printf("%s",result.c_str());
    if(result.find("FAIL")!=std::string::npos || result.find("TIMEOUT")!=std::string::npos)return 1;
    if(result.find("UNSUPPORTED:")!=std::string::npos)return 2;
    return result.find("PASS: ")==std::string::npos?1:0;
}
} // namespace diagnostics
