/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "dual_source.h"
#include "multidraw.h"
#include "multiviewport.h"
#include "clip_distance.h"
#include "vertex_stores.h"
#include "shader_policy.h"
#include "bc_policy.h"
#include "../layer/layer.h"
#include <string.h>

int hybris_clip_enabled(void) { return 1; }

int hybris_clip_active(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    VkPhysicalDeviceFeatures features;
    PFN_vkGetPhysicalDeviceFeatures query;
    if (!hybris_clip_enabled()) return 0;
    if (!hybris_layer_lookup_physical(physical, &context)) return 0;
    query = (PFN_vkGetPhysicalDeviceFeatures)context.resolver(context.instance, "vkGetPhysicalDeviceFeatures");
    if (!query) return 0;
    query(physical, &features);
    return features.shaderClipDistance == VK_FALSE;
}

int hybris_cull_active(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    VkPhysicalDeviceFeatures features;
    if (!hybris_layer_lookup_physical(physical, &context)) return 0;
    PFN_vkGetPhysicalDeviceFeatures query = (PFN_vkGetPhysicalDeviceFeatures)
        context.resolver(context.instance, "vkGetPhysicalDeviceFeatures");
    if (!query) return 0;
    query(physical, &features);
    return !features.shaderCullDistance;
}

int hybris_stage_point_active(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    VkPhysicalDeviceFeatures features;
    if (!hybris_layer_lookup_physical(physical, &context)) return 0;
    PFN_vkGetPhysicalDeviceFeatures query = (PFN_vkGetPhysicalDeviceFeatures)
        context.resolver(context.instance, "vkGetPhysicalDeviceFeatures");
    if (!query) return 0;
    query(physical, &features);
    return !features.shaderTessellationAndGeometryPointSize;
}

static PFN_vkVoidFunction backend_proc(VkPhysicalDevice physical, const char *name)
{
    struct hybris_layer_physical context;
    PFN_vkVoidFunction inner;
    if (!hybris_layer_lookup_physical(physical, &context)) return NULL;
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
    if (hybris_cull_active(physical)) out->shaderCullDistance = VK_TRUE;
    if (hybris_stage_point_active(physical)) out->shaderTessellationAndGeometryPointSize = VK_TRUE;
    if (hybris_vertex_stores_active(physical)) out->vertexPipelineStoresAndAtomics = VK_TRUE;
}

static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out, const char *name)
{
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)backend_proc(physical, name);
    if (!query) return;
    query(physical, out);
    if (hybris_clip_active(physical)) out->features.shaderClipDistance = VK_TRUE;
    if (hybris_cull_active(physical)) out->features.shaderCullDistance = VK_TRUE;
    if (hybris_stage_point_active(physical)) out->features.shaderTessellationAndGeometryPointSize = VK_TRUE;
    if (hybris_vertex_stores_active(physical)) {
        out->features.vertexPipelineStoresAndAtomics = VK_TRUE;
        /* Preserve native transform feedback. Pipelines needing discard
         * compensation and Xfb are rejected by the SPIR-V conversion rather
         * than disabling unrelated native capture pipelines device-wide. */
    }
}

static void VKAPI_CALL features2_core(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2"); }
static void VKAPI_CALL features2_khr(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2KHR"); }

static void apply_limits(VkPhysicalDevice physical, VkPhysicalDeviceLimits *limits)
{
    if(hybris_dual_source_active(physical) && limits->maxFragmentDualSrcAttachments<1)limits->maxFragmentDualSrcAttachments=1;
    if(hybris_multidraw_active(physical))limits->maxDrawIndirectCount=UINT32_MAX;
    if(hybris_multiviewport_active(physical) && limits->maxViewports<16)limits->maxViewports=16;
    if (!hybris_clip_active(physical) && !hybris_cull_active(physical)) return;
    if (hybris_clip_active(physical) && limits->maxClipDistances < 8) limits->maxClipDistances = 8;
    if (hybris_cull_active(physical) && limits->maxCullDistances < 8) limits->maxCullDistances = 8;
    if (limits->maxCombinedClipAndCullDistances < limits->maxClipDistances)
        limits->maxCombinedClipAndCullDistances = limits->maxClipDistances;
    if (limits->maxCombinedClipAndCullDistances < limits->maxCullDistances)
        limits->maxCombinedClipAndCullDistances = limits->maxCullDistances;
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
