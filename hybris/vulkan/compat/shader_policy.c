/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "shader_policy.h"
#include "scaled_formats.h"
#include "bc_policy.h"
#include "../icd/wsi.h"
#include <string.h>

/* Static vertex conversion cannot select a shader from command-time vertex
 * formats or independently compiled graphics libraries/shader objects. Keep
 * those optional paths unavailable only when this physical device actually
 * needs conversion (or conversion is explicitly forced). */
unsigned hybris_shader_physical_mask(VkPhysicalDevice physical)
{
    struct hybris_icd_physical context;
    if (!hybris_scaled_enabled() || !hybris_icd_lookup_physical(physical, &context)) return 0;
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
    if (!hybris_shader_physical_mask(physical)) return VK_SUCCESS;
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
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
    /* Compose with BC's filtering when both experimental policies are active;
     * it in turn resolves the original physical-device query. */
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)hybris_bc_policy_proc(name);
    if (!query) query = (PFN_vkGetPhysicalDeviceFeatures2)context.resolver(context.instance, name);
    query(physical, out);
    if (!hybris_shader_physical_mask(physical)) return;
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
PFN_vkVoidFunction hybris_shader_policy_proc(const char *name)
{
    if (!hybris_scaled_enabled()) return NULL;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)features2_core;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)features2_khr;
    return NULL;
}
