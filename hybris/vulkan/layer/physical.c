/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "layer.h"
#include "../compat/vertex_stores.h"
#include "../compat/shader_dispatch.h"
#include "../compat/shader_policy.h"
#include "../compat/bc_policy.h"
#include "../compat/clip_distance.h"
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
    int vertex_stores = !layer && hybris_vertex_stores_active(physical);
    if (layer || (!bc_mask && !shader_mask && !vertex_stores))
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
        if (vertex_stores && !strcmp(all[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) continue;
        if ((bc_mask && !hybris_bc_extension_allowed(all[i].extensionName)) ||
            ((shader_mask || vertex_stores) && !hybris_shader_extension_allowed(all[i].extensionName))) continue;
        if (properties && written < capacity) properties[written++] = all[i];
        ++kept;
    }
    free(all);
    *count = properties ? written : kept;
    return properties && written < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

PFN_vkVoidFunction hybris_layer_physical_proc(const char *name)
{
    PFN_vkVoidFunction function = hybris_clip_policy_proc(name);
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
