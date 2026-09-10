/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "shader_policy.h"
#include "vertex_stores.h"
#include "scaled_formats.h"
#include "bc_policy.h"
#include "../layer/layer.h"
#include <string.h>
#include <stdlib.h>

/* Static vertex conversion cannot select a shader from command-time vertex
 * formats or independently compiled graphics libraries/shader objects. Keep
 * those optional paths unavailable only when this physical device actually
 * needs conversion (or conversion is explicitly forced). */
unsigned hybris_shader_physical_mask(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    if (!hybris_scaled_enabled() || !hybris_layer_lookup_physical(physical, &context)) return 0;
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceFormatProperties");
    return hybris_scaled_physical_mask(physical, query);
}
int hybris_shader_extension_allowed(const char *name)
{
    return strcmp(name, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) &&
        strcmp(name, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
        strcmp(name, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
}
int hybris_shader_command_allowed(const char *name)
{
    return strcmp(name, "vkCmdSetVertexInputEXT") && strcmp(name, "vkCreateShadersEXT") &&
        strcmp(name, "vkDestroyShaderEXT") && strcmp(name, "vkGetShaderBinaryDataEXT") &&
        strcmp(name, "vkCmdBindShadersEXT");
}
VkResult hybris_shader_prepare_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info)
{
    if (!hybris_shader_physical_mask(physical) && !hybris_vertex_stores_active(physical)) return VK_SUCCESS;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (!hybris_shader_extension_allowed(info->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        VkBool32 enabled = VK_FALSE;
        switch (node->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
            enabled = ((const VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)node)->vertexInputDynamicState;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
            enabled = ((const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)node)->graphicsPipelineLibrary;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
            enabled = ((const VkPhysicalDeviceShaderObjectFeaturesEXT *)node)->shaderObject;
            break;
        default: break;
        }
        if (enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return VK_SUCCESS;
}
static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out, const char *name)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    /* Compose with BC's filtering when both experimental policies are active;
     * it in turn resolves the original physical-device query. */
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)hybris_bc_policy_proc(name);
    if (!query) query = (PFN_vkGetPhysicalDeviceFeatures2)context.resolver(context.instance, name);
    query(physical, out);
    if (!hybris_shader_physical_mask(physical) && !hybris_vertex_stores_active(physical)) return;
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext) {
        switch (node->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT:
            ((VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)node)->vertexInputDynamicState = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT:
            ((VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)node)->graphicsPipelineLibrary = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
            ((VkPhysicalDeviceShaderObjectFeaturesEXT *)node)->shaderObject = VK_FALSE;
            break;
        default: break;
        }
    }
}
static void VKAPI_CALL features2_core(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2"); }
static void VKAPI_CALL features2_khr(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2KHR"); }
/* KHR promotion added supportsNonZeroFirstInstance and a distinct properties
 * sType. Some clients enable KHR but still request the old EXT max field. On
 * KHR-only drivers, supply that field from its real KHR value without exposing
 * EXT or promising EXT's nonzero-firstInstance behavior. */
static void properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, const char *name)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceProperties2 query = (PFN_vkGetPhysicalDeviceProperties2)context.resolver(context.instance, name);
    query(physical, out);
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *legacy = NULL;
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT)
            legacy = (void *)node;
    if (!legacy || legacy->maxVertexAttribDivisor || !hybris_shader_physical_mask(physical)) return;
    PFN_vkEnumerateDeviceExtensionProperties enumerate = (PFN_vkEnumerateDeviceExtensionProperties)
        context.resolver(context.instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t count = 0;
    if (enumerate(physical, NULL, &count, NULL) != VK_SUCCESS || !count) return;
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return;
    int khr = 0, ext = 0;
    if (enumerate(physical, NULL, &count, extensions) == VK_SUCCESS)
        for (uint32_t j = 0; j < count; ++j) {
            khr |= !strcmp(extensions[j].extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
            ext |= !strcmp(extensions[j].extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        }
    free(extensions);
    if (!khr || ext) return;
    VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR divisor = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 native = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &divisor};
    query(physical, &native);
    legacy->maxVertexAttribDivisor = divisor.maxVertexAttribDivisor;
}
static void VKAPI_CALL properties2_core(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ properties2(physical, out, "vkGetPhysicalDeviceProperties2"); }
static void VKAPI_CALL properties2_khr(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ properties2(physical, out, "vkGetPhysicalDeviceProperties2KHR"); }
PFN_vkVoidFunction hybris_shader_policy_proc(const char *name)
{
    /* Vertex-store discard conversion also requires complete static pipelines. */
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)properties2_core;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)properties2_khr;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)features2_core;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)features2_khr;
    return NULL;
}
