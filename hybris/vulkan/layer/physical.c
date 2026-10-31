/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "../compat/dynamic_rendering.h"
#include "../compat/maintenance5.h"
#include "../compat/dual_source.h"
#include "layer.h"
#include "../compat/vertex_stores.h"
#include "../compat/shader_dispatch.h"
#include "../compat/shader_policy.h"
#include "../compat/bc_policy.h"
#include "../compat/clip_distance.h"
#include "../compat/robustness2.h"
#include <stdlib.h>
#include <string.h>

static void VKAPI_CALL format_properties(VkPhysicalDevice physical, VkFormat format,
                                         VkFormatProperties *properties)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceFormatProperties");
    query(physical, format, properties);
    hybris_scaled_format(query, physical, format, properties);
    hybris_bc_format_properties(physical, format, properties);
}
static void format_properties2(VkPhysicalDevice physical, VkFormat format,
                               VkFormatProperties2 *properties, const char *name)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceFormatProperties2 query2 = (PFN_vkGetPhysicalDeviceFormatProperties2)
        context.resolver(context.instance, name);
    query2(physical, format, properties);
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceFormatProperties");
    VkFormatFeatureFlags before = properties->formatProperties.bufferFeatures;
    hybris_scaled_format(query, physical, format, &properties->formatProperties);
    if (!(before & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) &&
        (properties->formatProperties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
        for (VkBaseOutStructure *next = properties->pNext; next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
                ((VkFormatProperties3 *)next)->bufferFeatures |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
    if (hybris_bc_format_properties(physical, format, &properties->formatProperties)) {
        for (VkBaseOutStructure *next = properties->pNext; next; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
                VkFormatProperties3 *extended = (void *)next;
                extended->linearTilingFeatures = extended->bufferFeatures = 0;
                extended->optimalTilingFeatures = properties->formatProperties.optimalTilingFeatures;
            } else if (next->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
                ((VkDrmFormatModifierPropertiesListEXT *)next)->drmFormatModifierCount = 0;
            } else if (next->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT) {
                ((VkDrmFormatModifierPropertiesList2EXT *)next)->drmFormatModifierCount = 0;
            }
        }
    }
}
static void VKAPI_CALL format_properties2_core(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *properties)
{ format_properties2(physical, format, properties, "vkGetPhysicalDeviceFormatProperties2"); }
static void VKAPI_CALL format_properties2_khr(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *properties)
{ format_properties2(physical, format, properties, "vkGetPhysicalDeviceFormatProperties2KHR"); }

static VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice physical,
    const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateDeviceExtensionProperties enumerate = (PFN_vkEnumerateDeviceExtensionProperties)
        context.resolver(context.instance, "vkEnumerateDeviceExtensionProperties");
    unsigned bc_mask = layer ? 0 : hybris_bc_physical_mask(physical);
    unsigned shader_mask = layer ? 0 : hybris_shader_physical_mask(physical);
    int pipeline_compat = !layer && (hybris_dual_source_active(physical) || hybris_shader_polygon_emulated(physical));
    int vertex_stores = !layer && hybris_vertex_stores_active(physical);
    int dynamic = !layer && hybris_dynamic_emulated(physical);
    int maintenance5 = !layer && hybris_maintenance5_emulated(physical);
    struct hybris_robustness2_policy robust={0};
    if(!layer) {
        VkResult policy_result=hybris_robustness2_policy(physical,&robust);
        if(policy_result!=VK_SUCCESS)return policy_result;
    }
    if (layer || (!bc_mask && !shader_mask && !pipeline_compat && !vertex_stores && !robust.emulated && !dynamic && !maintenance5))
        return enumerate(physical, layer, count, properties);
    uint32_t available = 0;
    VkResult result = enumerate(physical, NULL, &available, NULL);
    if (result != VK_SUCCESS) return result;
    VkExtensionProperties *all = available ? calloc(available, sizeof(*all)) : NULL;
    if (available && !all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = enumerate(physical, NULL, &available, all);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) { free(all); return result; }
    uint32_t kept = 0, written = 0, capacity = properties ? *count : 0;
    for (uint32_t i = 0; i < available; ++i) {
        if(dynamic && (!strcmp(all[i].extensionName,"VK_KHR_dynamic_rendering_local_read") ||
            !strcmp(all[i].extensionName,"VK_EXT_dynamic_rendering_unused_attachments")))continue;
        if((robust.emulated & HYBRIS_ROBUST_NULL) &&
            (!strcmp(all[i].extensionName,"VK_EXT_descriptor_buffer") ||
             !strcmp(all[i].extensionName,"VK_EXT_shader_object") ||
             !strcmp(all[i].extensionName,"VK_KHR_acceleration_structure") ||
             !strcmp(all[i].extensionName,"VK_KHR_ray_tracing_pipeline") ||
             !strcmp(all[i].extensionName,"VK_KHR_ray_query")))continue;
        if ((bc_mask && !hybris_bc_extension_allowed(all[i].extensionName)) ||
            ((shader_mask || pipeline_compat || vertex_stores) && !hybris_shader_extension_allowed(all[i].extensionName))) continue;
        if (properties && written < capacity) properties[written++] = all[i];
        ++kept;
    }
    if(robust.emulated && !robust.native_extensions) {
        if(properties && written<capacity) {
            properties[written]=(VkExtensionProperties){.extensionName=VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,.specVersion=1};
            ++written;
        }
        ++kept;
    }
    if(dynamic && !hybris_dynamic_native_extension(physical,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
        if(properties && written<capacity)properties[written++]=(VkExtensionProperties){.extensionName=VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,.specVersion=1};
        ++kept;
    }
    if(maintenance5 && !hybris_dynamic_native_extension(physical,VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
        if(properties && written<capacity)properties[written++]=(VkExtensionProperties){.extensionName=VK_KHR_MAINTENANCE_5_EXTENSION_NAME,.specVersion=1};
        ++kept;
    }
    free(all);
    *count = properties ? written : kept;
    return properties && written < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

PFN_vkVoidFunction hybris_layer_physical_proc(const char *name)
{
    PFN_vkVoidFunction function = hybris_robustness2_physical_proc(name);
    if(!function)function = hybris_clip_policy_proc(name);
    if (!function) function = hybris_shader_policy_proc(name);
    if (!function) function = hybris_bc_policy_proc(name);
    if (function) return function;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)enumerate_device_extensions;
    if (hybris_scaled_enabled() || hybris_bc_enabled()) {
        if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)format_properties;
        if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2")) return (PFN_vkVoidFunction)format_properties2_core;
        if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR")) return (PFN_vkVoidFunction)format_properties2_khr;
    }
    return NULL;
}
