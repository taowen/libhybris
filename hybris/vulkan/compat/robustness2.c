/* SPDX-License-Identifier: GPL-3.0-or-later
 * Compatibility policy follows MaliBridge 95a1e600a51299294e06bb09db2db8cc509fe6a3,
 * server/server_emu.c and server/server_instance.c (ASEbrahim and contributors).
 * Core robustness is a weaker substitute, not robustness2 conformance.
 */
#define VK_NO_PROTOTYPES
#include "robustness2.h"
#include "shader_policy.h"
#include "../layer/layer.h"
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

VkResult hybris_robustness2_policy(VkPhysicalDevice physical,struct hybris_robustness2_policy *out)
{
    *out=(struct hybris_robustness2_policy){.native={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT}};
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateDeviceExtensionProperties enumerate=(void*)owner.resolver(owner.instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t count=0;
    VkResult result=enumerate(physical,NULL,&count,NULL);
    if(result!=VK_SUCCESS)return result;
    VkExtensionProperties *extensions=count?calloc(count,sizeof(*extensions)):NULL;
    if(count && !extensions)return VK_ERROR_OUT_OF_HOST_MEMORY;
    result=enumerate(physical,NULL,&count,extensions);
    if(result!=VK_SUCCESS){free(extensions);return result;}
    for(uint32_t i=0;i<count;++i) {
        out->native_extensions|=!strcmp(extensions[i].extensionName,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)?1:0;
        out->native_extensions|=!strcmp(extensions[i].extensionName,VK_KHR_ROBUSTNESS_2_EXTENSION_NAME)?2:0;
        out->image_extension|=!strcmp(extensions[i].extensionName,VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
    }
    free(extensions);
    PFN_vkGetPhysicalDeviceFeatures get=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFeatures2 get2=(void*)owner.resolver(owner.instance,"vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceFeatures core;
    get(physical,&core);out->core_buffer=core.robustBufferAccess;
    VkPhysicalDeviceProperties properties;
    ((PFN_vkGetPhysicalDeviceProperties)owner.resolver(owner.instance,"vkGetPhysicalDeviceProperties"))(physical,&properties);
    if(!get2 || owner.api_version<VK_API_VERSION_1_1 || properties.apiVersion<VK_API_VERSION_1_1)return VK_SUCCESS;
    if(out->native_extensions) {
        VkPhysicalDeviceFeatures2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&out->native};
        get2(physical,&query);
    }
    if(out->image_extension || (owner.api_version>=VK_API_VERSION_1_3 && properties.apiVersion>=VK_API_VERSION_1_3)) {
        VkPhysicalDeviceImageRobustnessFeatures image={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES};
        VkPhysicalDeviceFeatures2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&image};
        get2(physical,&query);out->core_image=image.robustImageAccess;
    }
    const char *mode=getauxval(AT_SECURE)?NULL:getenv("HYBRIS_ROBUSTNESS2");
    if((mode && !strcmp(mode,"0")) || !out->core_buffer)return VK_SUCCESS;
    int force=mode && !strcmp(mode,"force");
    out->emulated=(!out->native.robustBufferAccess2 || force?HYBRIS_ROBUST_BUFFER:0) |
        (!out->native.robustImageAccess2 || force?HYBRIS_ROBUST_IMAGE:0) |
        (!out->native.nullDescriptor || force?HYBRIS_ROBUST_NULL:0);
    if(out->emulated & HYBRIS_ROBUST_NULL) {
        uint32_t families=0;
        ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)owner.resolver(owner.instance,"vkGetPhysicalDeviceQueueFamilyProperties"))(physical,&families,NULL);
        if(!families || families>16)out->emulated=0;
    }
    return VK_SUCCESS;
}

void hybris_robustness2_features(VkPhysicalDevice physical,VkPhysicalDeviceFeatures2 *features)
{
    struct hybris_robustness2_policy policy;
    if(hybris_robustness2_policy(physical,&policy)!=VK_SUCCESS || !policy.emulated)return;
    for(VkBaseOutStructure *node=features->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *r=(void*)node;
            r->robustBufferAccess2=policy.native.robustBufferAccess2 || (policy.emulated & HYBRIS_ROBUST_BUFFER);
            r->robustImageAccess2=policy.native.robustImageAccess2 || (policy.emulated & HYBRIS_ROBUST_IMAGE);
            r->nullDescriptor=policy.native.nullDescriptor || (policy.emulated & HYBRIS_ROBUST_NULL);
        } else if(policy.emulated & HYBRIS_ROBUST_NULL) {
            switch(node->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT: {
                VkPhysicalDeviceDescriptorBufferFeaturesEXT *f=(void*)node;
                f->descriptorBuffer=f->descriptorBufferCaptureReplay=f->descriptorBufferImageLayoutIgnored=f->descriptorBufferPushDescriptors=VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
                ((VkPhysicalDeviceShaderObjectFeaturesEXT*)node)->shaderObject=VK_FALSE;break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR:
                ((VkPhysicalDeviceRayQueryFeaturesKHR*)node)->rayQuery=VK_FALSE;break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR: {
                VkPhysicalDeviceAccelerationStructureFeaturesKHR *f=(void*)node;
                f->accelerationStructure=f->accelerationStructureCaptureReplay=f->accelerationStructureIndirectBuild=f->accelerationStructureHostCommands=f->descriptorBindingAccelerationStructureUpdateAfterBind=VK_FALSE;break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR: {
                VkPhysicalDeviceRayTracingPipelineFeaturesKHR *f=(void*)node;
                f->rayTracingPipeline=f->rayTracingPipelineShaderGroupHandleCaptureReplay=f->rayTracingPipelineShaderGroupHandleCaptureReplayMixed=f->rayTracingPipelineTraceRaysIndirect=f->rayTraversalPrimitiveCulling=VK_FALSE;break;
            }
            default:break;
            }
        }
}

static void properties2(VkPhysicalDevice physical,VkPhysicalDeviceProperties2 *out,const char *name)
{
    struct hybris_layer_physical owner;
    if(!hybris_layer_lookup_physical(physical,&owner))return;
    PFN_vkGetPhysicalDeviceProperties2 query=(void*)hybris_shader_policy_proc(name);
    if(!query)query=(void*)owner.resolver(owner.instance,name);
    if(!query)return;
    query(physical,out);
    struct hybris_robustness2_policy policy;
    if(hybris_robustness2_policy(physical,&policy)!=VK_SUCCESS || !policy.emulated)return;
    for(VkBaseOutStructure *node=out->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT) {
            VkPhysicalDeviceRobustness2PropertiesEXT *p=(void*)node;
            if(!policy.native_extensions || !p->robustStorageBufferAccessSizeAlignment)p->robustStorageBufferAccessSizeAlignment=4;
            if(!policy.native_extensions || !p->robustUniformBufferAccessSizeAlignment)p->robustUniformBufferAccessSizeAlignment=256;
        }
}
static void VKAPI_CALL core_properties(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *out)
{properties2(p,out,"vkGetPhysicalDeviceProperties2");}
static void VKAPI_CALL khr_properties(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *out)
{properties2(p,out,"vkGetPhysicalDeviceProperties2KHR");}
PFN_vkVoidFunction hybris_robustness2_physical_proc(const char *name)
{
    if(!strcmp(name,"vkGetPhysicalDeviceProperties2"))return (PFN_vkVoidFunction)core_properties;
    if(!strcmp(name,"vkGetPhysicalDeviceProperties2KHR"))return (PFN_vkVoidFunction)khr_properties;
    return NULL;
}
