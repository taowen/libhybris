/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "clip_distance.h"
#include "shader_policy.h"
#include "bc_policy.h"
#include "../icd/wsi.h"
#include <string.h>

int hybris_clip_enabled(void) { return 1; }

int hybris_clip_active(VkPhysicalDevice physical)
{
    struct hybris_icd_physical context;
    VkPhysicalDeviceFeatures features;
    PFN_vkGetPhysicalDeviceFeatures query;
    if (!hybris_clip_enabled()) return 0;
    if (!hybris_icd_lookup_physical(physical, &context)) return 0;
    query = (PFN_vkGetPhysicalDeviceFeatures)context.resolver(context.instance, "vkGetPhysicalDeviceFeatures");
    if (!query) return 0;
    query(physical, &features);
    return features.shaderClipDistance == VK_FALSE;
}

int hybris_clip_pipeline(const VkGraphicsPipelineCreateInfo *info)
{
    unsigned vertex = 0, fragment = 0;
    /* Shader rewrite does not depend on pipeline pNext (dynamic rendering,
     * creation feedback). Skipping on any pNext left ClipDistance in the HAL
     * modules of every Blender graphics pipeline. */
    if (!info || (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)) return 0;
    for (uint32_t i = 0; i < info->stageCount; ++i) {
        if (info->pStages[i].pNext) return 0;
        vertex += info->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT;
        fragment += info->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT;
        if (info->pStages[i].stage != VK_SHADER_STAGE_VERTEX_BIT &&
            info->pStages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT) return 0;
    }
    return vertex == 1 && fragment == 1;
}

void hybris_clip_filter_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *features)
{
    if (features && hybris_clip_active(physical)) features->shaderClipDistance = VK_FALSE;
}

static PFN_vkVoidFunction backend_proc(VkPhysicalDevice physical, const char *name)
{
    struct hybris_icd_physical context;
    PFN_vkVoidFunction inner;
    if (!hybris_icd_lookup_physical(physical, &context)) return NULL;
    inner = hybris_shader_policy_proc(name);
    if (inner) return inner;
    inner = hybris_bc_policy_proc(name);
    return inner ? inner : context.resolver(context.instance, name);
}

static void VKAPI_CALL features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *out)
{
    PFN_vkGetPhysicalDeviceFeatures query = (PFN_vkGetPhysicalDeviceFeatures)backend_proc(physical, "vkGetPhysicalDeviceFeatures");
    if (!query) return;
    query(physical, out);
    if (hybris_clip_active(physical)) out->shaderClipDistance = VK_TRUE;
}

static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out, const char *name)
{
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)backend_proc(physical, name);
    if (!query) return;
    query(physical, out);
    if (hybris_clip_active(physical)) out->features.shaderClipDistance = VK_TRUE;
}

static void VKAPI_CALL features2_core(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2"); }
static void VKAPI_CALL features2_khr(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2KHR"); }

static void apply_limits(VkPhysicalDevice physical, VkPhysicalDeviceLimits *limits)
{
    if (!hybris_clip_active(physical)) return;
    if (limits->maxClipDistances < 8) limits->maxClipDistances = 8;
    if (limits->maxCombinedClipAndCullDistances < limits->maxClipDistances)
        limits->maxCombinedClipAndCullDistances = limits->maxClipDistances;
}

static void VKAPI_CALL properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *out)
{
    PFN_vkGetPhysicalDeviceProperties query = (PFN_vkGetPhysicalDeviceProperties)backend_proc(physical, "vkGetPhysicalDeviceProperties");
    if (!query) return;
    query(physical, out);
    apply_limits(physical, &out->limits);
}

static void properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out, const char *name)
{
    PFN_vkGetPhysicalDeviceProperties2 query = (PFN_vkGetPhysicalDeviceProperties2)backend_proc(physical, name);
    if (!query) return;
    query(physical, out);
    apply_limits(physical, &out->properties.limits);
}

static void VKAPI_CALL properties2_core(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ properties2(physical, out, "vkGetPhysicalDeviceProperties2"); }
static void VKAPI_CALL properties2_khr(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{ properties2(physical, out, "vkGetPhysicalDeviceProperties2KHR"); }

PFN_vkVoidFunction hybris_clip_policy_proc(const char *name)
{
    if (!hybris_clip_enabled()) return NULL;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures")) return (PFN_vkVoidFunction)features;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)features2_core;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)features2_khr;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)properties;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)properties2_core;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)properties2_khr;
    return NULL;
}
