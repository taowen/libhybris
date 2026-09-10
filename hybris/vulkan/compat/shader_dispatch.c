/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "shader_dispatch.h"
#include "scaled_vertex.h"
#include "scaled_formats.h"
#include "spirv_entry.h"
#include "shader_cleanup.h"
#include "spirv_builtins.h"
#include "point_size.h"
#include "spirv_inout.h"
#include "spirv_image_bounds.h"
#include "clip_distance.h"
#include "spirv_builtins.h"
#include "shader_policy.h"
#include "vertex_stores.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>


struct shader {
    VkShaderModule handle;
    size_t size;
    int extensions;
    uint32_t *code;
    VkAllocationCallbacks allocator;
    int custom;
    struct shader *next;
};
struct shader_device {
    VkDevice handle;
    PFN_vkCreateShaderModule create_shader;
    PFN_vkDestroyShaderModule destroy_shader;
    PFN_vkCreateGraphicsPipelines create_pipelines;
    unsigned mask;
    int point_size;
    int inout;
    int image_bounds;
    int clip;
    int vertex_stores;
    VkAllocationCallbacks allocator;
    int custom;
    struct shader *shaders;
    struct shader_device *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct shader_device *devices;
static _Atomic unsigned point_reports;
int hybris_shader_enabled(void)
{
    return hybris_scaled_enabled() || hybris_point_size_enabled() ||
        hybris_inout_enabled() || hybris_image_bounds_enabled() || hybris_clip_enabled();
}
static struct shader_device *find_device(VkDevice handle)
{
    pthread_mutex_lock(&guard);
    struct shader_device *device = devices;
    while (device && device->handle != handle) device = device->next;
    pthread_mutex_unlock(&guard);
    return device;
}
int hybris_shader_device_proc_allowed(VkDevice handle, const char *name)
{
    if (hybris_shader_command_allowed(name)) return 1;
    struct shader_device *device = find_device(handle);
    return !device || (!device->mask && !device->vertex_stores);
}
VkResult hybris_shader_device_create(VkDevice handle, VkPhysicalDevice physical,
    PFN_vkGetDeviceProcAddr resolver, PFN_vkGetPhysicalDeviceFormatProperties query,
    const VkAllocationCallbacks *allocator)
{
    if (!hybris_shader_enabled()) return VK_SUCCESS;
    struct shader_device *device = hybris_scaled_alloc(allocator, sizeof(*device), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!device) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(device, 0, sizeof(*device));
    device->handle = handle;
    device->custom = allocator != NULL;
    if (allocator) device->allocator = *allocator;
    device->create_shader = hybris_shader_cleanup_enabled() ? hybris_shader_cleanup_create :
        (PFN_vkCreateShaderModule)resolver(handle, "vkCreateShaderModule");
    device->destroy_shader = (PFN_vkDestroyShaderModule)resolver(handle, "vkDestroyShaderModule");
    device->create_pipelines = (PFN_vkCreateGraphicsPipelines)resolver(handle, "vkCreateGraphicsPipelines");
    device->mask = hybris_scaled_mask(handle, physical, query);
    device->point_size = hybris_point_size_enabled();
    device->inout = hybris_inout_enabled();
    device->image_bounds = hybris_image_bounds_enabled();
    device->clip = hybris_clip_active(physical);
    device->vertex_stores = hybris_vertex_stores_active(physical);
    pthread_mutex_lock(&guard);
    device->next = devices;
    devices = device;
    pthread_mutex_unlock(&guard);
    return VK_SUCCESS;
}
static void free_shader(struct shader *shader)
{
    const VkAllocationCallbacks *allocator = shader->custom ? &shader->allocator : NULL;
    hybris_scaled_free(allocator, shader->code);
    hybris_scaled_free(allocator, shader);
}
void hybris_shader_device_destroy(VkDevice handle)
{
    pthread_mutex_lock(&guard);
    struct shader_device **link = &devices;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    struct shader_device *device = *link;
    if (device) *link = device->next;
    pthread_mutex_unlock(&guard);
    if (!device) return;
    while (device->shaders) {
        struct shader *shader = device->shaders;
        device->shaders = shader->next;
        free_shader(shader);
    }
    hybris_scaled_free(device->custom ? &device->allocator : NULL, device);
}
static VkResult VKAPI_CALL create_shader(VkDevice handle, const VkShaderModuleCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkShaderModule *module)
{
    struct shader_device *device = find_device(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->mask && !device->point_size && !device->inout && !device->image_bounds &&
        !device->clip && !device->vertex_stores)
        return device->create_shader(handle, info, allocator, module);
    struct shader *shader = hybris_scaled_alloc(allocator, sizeof(*shader), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!shader) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(shader, 0, sizeof(*shader));
    shader->custom = allocator != NULL;
    if (allocator) shader->allocator = *allocator;
    shader->code = hybris_scaled_alloc(allocator, info->codeSize, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!shader->code) { free_shader(shader); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    shader->size = info->codeSize;
    shader->extensions = info->pNext != NULL;
    memcpy(shader->code, info->pCode, info->codeSize);
    VkShaderModuleCreateInfo created = *info;
    uint32_t *bounded = NULL;
    size_t bounded_size = 0;
    unsigned collapsed = 0;
    VkResult result = VK_SUCCESS;
    if (device->image_bounds && !info->pNext)
        result = hybris_spirv_image_bounds(info->pCode, info->codeSize, allocator, &bounded, &bounded_size, &collapsed);
    if (result != VK_SUCCESS) { free_shader(shader); return result; }
    if (bounded) {
        created.pCode = bounded;
        created.codeSize = bounded_size;
    }
    uint32_t *cleaned = NULL;
    size_t cleaned_size = 0;
    unsigned removed = 0;
    if (result == VK_SUCCESS && device->clip && !info->pNext)
        result = hybris_spirv_unused_clip_capabilities(created.pCode, created.codeSize, allocator,
            &cleaned, &cleaned_size, &removed);
    if (result != VK_SUCCESS) {
        hybris_scaled_free(allocator, bounded);
        free_shader(shader);
        return result;
    }
    if (cleaned) {
        created.pCode = cleaned;
        created.codeSize = cleaned_size;
    }
    result = device->create_shader(handle, &created, allocator, module);
    hybris_scaled_free(allocator, bounded);
    hybris_scaled_free(allocator, cleaned);
    if (result != VK_SUCCESS) { free_shader(shader); return result; }
    shader->handle = *module;
    pthread_mutex_lock(&guard);
    shader->next = device->shaders;
    device->shaders = shader;
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL destroy_shader(VkDevice handle, VkShaderModule module,
    const VkAllocationCallbacks *allocator)
{
    struct shader_device *device = find_device(handle);
    if (!device) return;
    pthread_mutex_lock(&guard);
    struct shader **link = &device->shaders;
    while (*link && (*link)->handle != module) link = &(*link)->next;
    struct shader *shader = *link;
    if (shader) *link = shader->next;
    pthread_mutex_unlock(&guard);
    device->destroy_shader(handle, module, allocator);
    if (shader) free_shader(shader);
}

struct pipeline_copy {
    VkPipelineVertexInputStateCreateInfo input;
    VkVertexInputAttributeDescription *attributes;
    VkPipelineShaderStageCreateInfo *stages;
    VkShaderModule *temporary;
    uint32_t stage_count;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineColorBlendAttachmentState *attachments;
    VkPipelineDynamicStateCreateInfo dynamic;
    VkDynamicState *dynamic_states;

};
/* Discard normally permits null downstream state. Supply inert state because
 * this driver needs rasterization enabled to execute pre-raster memory writes. */
static VkResult discard_state(struct shader_device *device, VkGraphicsPipelineCreateInfo *info,
    struct pipeline_copy *copy, const VkAllocationCallbacks *allocator)
{
    struct hybris_vertex_rendering rendering;
    VkResult result = hybris_vertex_rendering_info(device->handle, info, &rendering);
    if (result != VK_SUCCESS) return result;
    copy->raster = (VkPipelineRasterizationStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    copy->viewport = (VkViewport){0, 0, 1, 1, 0, 1};
    copy->scissor = (VkRect2D){{0, 0}, {1, 1}};
    copy->viewport_state = (VkPipelineViewportStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &copy->viewport,
        .scissorCount = 1, .pScissors = &copy->scissor};
    copy->multisample = (VkPipelineMultisampleStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = rendering.samples};
    copy->depth = (VkPipelineDepthStencilStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (rendering.colors) {
        copy->attachments = hybris_scaled_alloc(allocator, rendering.colors * sizeof(*copy->attachments),
            VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!copy->attachments) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memset(copy->attachments, 0, rendering.colors * sizeof(*copy->attachments));
    }
    copy->blend = (VkPipelineColorBlendStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = rendering.colors, .pAttachments = copy->attachments};
    if (info->pDynamicState) {
        copy->dynamic = *info->pDynamicState;
        copy->dynamic.dynamicStateCount = 0;
        uint32_t count = info->pDynamicState->dynamicStateCount;
        copy->dynamic_states = hybris_scaled_alloc(allocator, (count ? count : 1) * sizeof(VkDynamicState),
            VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (!copy->dynamic_states) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < count; ++i) {
            VkDynamicState state = info->pDynamicState->pDynamicStates[i];
            switch (state) {
            case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
            case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
            case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
            case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
                copy->dynamic_states[copy->dynamic.dynamicStateCount++] = state;
                break;
            default: break;
            }
        }
        copy->dynamic.pDynamicStates = copy->dynamic_states;
        info->pDynamicState = &copy->dynamic;
    }
    info->pRasterizationState = &copy->raster;
    info->pViewportState = &copy->viewport_state;
    info->pMultisampleState = &copy->multisample;
    info->pDepthStencilState = &copy->depth;
    info->pColorBlendState = &copy->blend;
    return VK_SUCCESS;
}
static VkResult convert_pipeline(struct shader_device *device, VkGraphicsPipelineCreateInfo *info,
    struct pipeline_copy *copy, const VkAllocationCallbacks *allocator, const char **reason)
{
    *reason = "unsupported pipeline state for experimental scaled conversion";
    /* These paths can ignore pVertexInputState entirely. Check them before
     * dereferencing it; dynamic input would also require command-time shader
     * variants, which this experimental pipeline-only fallback cannot supply. */
    int point = device->point_size && hybris_point_size_pipeline(info);
    int inout = device->inout && hybris_inout_pipeline(info);
    int clip = device->clip && hybris_clip_pipeline(info);
    uint32_t clip_location = 0, clip_count = 0;
    int discard = 0;
    VkShaderStageFlagBits last_stage = VK_SHADER_STAGE_VERTEX_BIT;
    if (device->vertex_stores) {
        int writes = 0, dynamic_discard = 0;
        if (info->pDynamicState) for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; ++i)
            dynamic_discard |= info->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;
        for (uint32_t i = 0; i < info->stageCount; ++i) {
            const VkPipelineShaderStageCreateInfo *stage = &info->pStages[i];
            if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            if (stage->stage == VK_SHADER_STAGE_GEOMETRY_BIT) last_stage = stage->stage;
            if (stage->stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT && last_stage != VK_SHADER_STAGE_GEOMETRY_BIT)
                last_stage = stage->stage;
            pthread_mutex_lock(&guard);
            struct shader *shader = device->shaders;
            while (shader && shader->handle != stage->module) shader = shader->next;
            pthread_mutex_unlock(&guard);
            if (!shader || shader->extensions || stage->pNext) {
                *reason = "vertex stores require captured SPIR-V stages";
                return VK_ERROR_UNKNOWN;
            }
            int stage_writes = 0;
            VkResult result = hybris_spirv_storage_writes(shader->code, shader->size, allocator, &stage_writes);
            if (result != VK_SUCCESS) return result;
            writes |= stage_writes;
        }
        if (writes && dynamic_discard) {
            *reason = "vertex stores with dynamic rasterizer discard are not implemented";
            return VK_ERROR_UNKNOWN;
        }
        discard = writes && info->pRasterizationState && info->pRasterizationState->rasterizerDiscardEnable;
        if (discard) { point = inout = clip = 0; }
    }
    if (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) {
        *reason = "vertex format conversion does not support graphics pipeline libraries";
        return (device->mask || device->vertex_stores) ? VK_ERROR_UNKNOWN : VK_SUCCESS;
    }
    int vertex_stage = 0;
    for (uint32_t j = 0; j < info->stageCount; ++j)
        vertex_stage |= info->pStages[j].stage == VK_SHADER_STAGE_VERTEX_BIT;
    if (!vertex_stage) return VK_SUCCESS;
    if (info->pDynamicState) for (uint32_t j = 0; j < info->pDynamicState->dynamicStateCount; ++j)
        if ((device->mask || device->vertex_stores) && info->pDynamicState->pDynamicStates[j] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) {
            *reason = "vertex format conversion does not support dynamic vertex input";
            return VK_ERROR_UNKNOWN;
        }
    const VkPipelineVertexInputStateCreateInfo *input = info->pVertexInputState;
    unsigned count = 0;
    if (input) for (uint32_t j = 0; j < input->vertexAttributeDescriptionCount; ++j)
        for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i)
            if ((device->mask & (1u << i)) && input->pVertexAttributeDescriptions[j].format == hybris_scaled_formats[i].scaled) ++count;
    if (!count && !point && !inout && !clip && !discard) return VK_SUCCESS;
    /* Scaled and integer fetch use the same bytes and binding cadence. Keep
     * the divisor chain (EXT/KHR aliases) intact for the backend; reject other
     * vertex-input extensions whose interaction has not been established. */
    for (const VkBaseInStructure *chain = count ? input->pNext : NULL; chain; chain = chain->pNext)
        if (chain->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
            *reason = "vertex format conversion does not support this vertex input extension chain";
            return VK_ERROR_UNKNOWN;
        }
    struct hybris_scaled_attribute *attrs = count ? hybris_scaled_alloc(allocator, count * sizeof(*attrs), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) : NULL;
    copy->attributes = count ? hybris_scaled_alloc(allocator, input->vertexAttributeDescriptionCount * sizeof(*copy->attributes), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) : NULL;
    copy->stages = hybris_scaled_alloc(allocator, (info->stageCount + 1) * sizeof(*copy->stages), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    copy->stage_count = info->stageCount;
    copy->temporary = hybris_scaled_alloc(allocator, (info->stageCount + 1) * sizeof(*copy->temporary), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (copy->temporary) memset(copy->temporary, 0, (info->stageCount + 1) * sizeof(*copy->temporary));
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((count && (!attrs || !copy->attributes)) || !copy->stages || !copy->temporary) goto done;
    if (count) memcpy(copy->attributes, input->pVertexAttributeDescriptions, input->vertexAttributeDescriptionCount * sizeof(*copy->attributes));
    memcpy(copy->stages, info->pStages, info->stageCount * sizeof(*copy->stages));
    count = 0;
    if (copy->attributes) for (uint32_t j = 0; j < input->vertexAttributeDescriptionCount; ++j)
        for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i)
            if ((device->mask & (1u << i)) && copy->attributes[j].format == hybris_scaled_formats[i].scaled) {
                attrs[count++] = (struct hybris_scaled_attribute){copy->attributes[j].location,
                    hybris_scaled_formats[i].is_signed, hybris_scaled_formats[i].rb_swizzle};
                copy->attributes[j].format = hybris_scaled_formats[i].integer;
                break;
            }
    result = VK_SUCCESS;
    if (clip) {
        struct shader *vs = NULL, *fs = NULL;
        pthread_mutex_lock(&guard);
        for (uint32_t s = 0; s < info->stageCount; ++s) {
            struct shader *found = device->shaders;
            while (found && found->handle != info->pStages[s].module) found = found->next;
            if (info->pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = found;
            if (info->pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = found;
        }
        pthread_mutex_unlock(&guard);
        if (vs && fs && !vs->extensions && !fs->extensions)
            result = hybris_spirv_clip_plan(vs->code, vs->size, fs->code, fs->size,
                &clip_location, &clip_count);
        if (result != VK_SUCCESS) goto done;
    }
    for (uint32_t j = 0; j < info->stageCount; ++j) {
        VkPipelineShaderStageCreateInfo *stage = &copy->stages[j];
        int vertex = stage->stage == VK_SHADER_STAGE_VERTEX_BIT;
        int fragment = stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT;
        pthread_mutex_lock(&guard);
        struct shader *shader = device->shaders;
        while (shader && shader->handle != stage->module) shader = shader->next;
        pthread_mutex_unlock(&guard);
        int stage_discard = discard && stage->stage == last_stage;
        int stage_clip = clip && clip_count && (vertex || fragment) && shader && !shader->extensions;
        /* Module destruction is externally synchronized against pipeline use. */
        if (!vertex && !inout && !stage_clip && !stage_discard &&
            (!count || !shader || !hybris_spirv_multiple(shader->code, shader->size))) continue;
        if ((!shader || stage->pNext) && !count && !inout && !stage_clip && !stage_discard) continue;
        if (!shader || stage->pNext) {
            *reason = "stage conversion requires a captured module without stage extensions";
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
        int stage_point = point && !shader->extensions;
        int stage_inout = inout && fragment && !shader->extensions;
        if (!count && !stage_point && !stage_inout && !stage_clip && !stage_discard) continue;
        uint32_t *code = NULL;
        size_t size = 0;
        if (vertex && count) result = hybris_scaled_spirv(shader->code, shader->size, stage->pName, attrs, count,
                                                stage->pSpecializationInfo, allocator, &code, &size, reason);
        else {
            uint32_t model = 0;
            while (model < 5 && (1u << model) != (uint32_t)stage->stage) ++model;
            result = model < 5 ? hybris_spirv_entry(shader->code, shader->size, model,
                stage->pName, allocator, &code, &size, reason) : VK_ERROR_UNKNOWN;
        }
        if (result == VK_ERROR_UNKNOWN && !count && !stage_inout && !stage_clip && !stage_discard) {
            result = VK_SUCCESS;
            continue;
        }
        if (result != VK_SUCCESS) goto done;
        if (vertex && stage_point) {
            uint32_t *trimmed = NULL;
            size_t trimmed_size = 0;
            unsigned removed = 0;
            result = hybris_spirv_point_size(code, size, allocator, &trimmed, &trimmed_size, &removed);
            if (result != VK_SUCCESS) { hybris_scaled_free(allocator, code); goto done; }
            if (trimmed) {
                unsigned slot = atomic_load(&point_reports);
                while (slot < 129 && !atomic_compare_exchange_weak(&point_reports, &slot, slot + 1)) {}
                if (slot < 128) fprintf(stderr, "HYBRIS_POINT_SIZE removed=%u topology=%u original=%zu converted=%zu\n",
                    removed, info->pInputAssemblyState->topology, size, trimmed_size);
                else if (slot == 128) fprintf(stderr, "HYBRIS_POINT_SIZE truncated\n");
                hybris_scaled_free(allocator, code); code = trimmed; size = trimmed_size;
            } else if (!count && !stage_clip) { hybris_scaled_free(allocator, code); continue; }
        }
        if (stage_inout) {
            struct shader *vs = NULL;
            pthread_mutex_lock(&guard);
            for (uint32_t s = 0; s < info->stageCount; ++s) {
                if (info->pStages[s].stage != VK_SHADER_STAGE_VERTEX_BIT) continue;
                vs = device->shaders;
                while (vs && vs->handle != info->pStages[s].module) vs = vs->next;
            }
            pthread_mutex_unlock(&guard);
            if (!vs) {
                *reason = "inout matching needs a captured vertex module";
                result = VK_ERROR_UNKNOWN;
                hybris_scaled_free(allocator, code);
                goto done;
            }
            uint32_t *wide = NULL;
            size_t wide_size = 0;
            unsigned widened = 0;
            result = hybris_spirv_inout(vs->code, vs->size, code, size, allocator, &wide, &wide_size, &widened);
            if (result != VK_SUCCESS) { hybris_scaled_free(allocator, code); goto done; }
            if (wide) { hybris_scaled_free(allocator, code); code = wide; size = wide_size; }
            else if (!count && !stage_point && !stage_clip) { hybris_scaled_free(allocator, code); continue; }
        }
        if (hybris_shader_cleanup_enabled()) {
            uint32_t *clean = NULL;
            size_t clean_size = 0;
            unsigned removed = 0;
            result = hybris_spirv_unused_builtins(code, size, allocator, &clean, &clean_size, &removed);
            if (result != VK_SUCCESS) { hybris_scaled_free(allocator, code); goto done; }
            if (clean) { hybris_scaled_free(allocator, code); code = clean; size = clean_size; }
        }
        if (device->image_bounds) {
            uint32_t *bounded = NULL;
            size_t bounded_size = 0;
            unsigned collapsed = 0;
            result = hybris_spirv_image_bounds(code, size, allocator, &bounded, &bounded_size, &collapsed);
            if (result != VK_SUCCESS) { hybris_scaled_free(allocator, code); goto done; }
            if (bounded) { hybris_scaled_free(allocator, code); code = bounded; size = bounded_size; }
        }
        if (stage_clip) {
            uint32_t *clipped = NULL;
            size_t clipped_size = 0;
            unsigned made = 0;
            const uint32_t *src = code ? code : shader->code;
            size_t src_size = code ? size : shader->size;
            result = vertex ? hybris_spirv_clip_vertex(src, src_size, clip_location, allocator,
                    &clipped, &clipped_size, &made) :
                hybris_spirv_clip_fragment(src, src_size, clip_location, clip_count, allocator,
                    &clipped, &clipped_size, &made);
            if (result != VK_SUCCESS) { hybris_scaled_free(allocator, code); goto done; }
            if (clipped) {
                hybris_scaled_free(allocator, code);
                code = clipped;
                size = clipped_size;
            }
        }
        if (stage_discard) {
            uint32_t *clipped = NULL;
            size_t clipped_size = 0;
            uint32_t model = last_stage == VK_SHADER_STAGE_GEOMETRY_BIT ? 3 :
                last_stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ? 2 : 0;
            result = hybris_spirv_discard(code, size, model, stage->pName, allocator,
                &clipped, &clipped_size, reason);
            hybris_scaled_free(allocator, code);
            if (result != VK_SUCCESS) goto done;
            code = clipped; size = clipped_size;
        }
        if (!code) continue;
        hybris_scaled_dump(shader->code, shader->size, code, size, vertex ? attrs : NULL, vertex ? count : 0, stage->pSpecializationInfo);
        VkShaderModuleCreateInfo module = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
        result = device->create_shader(device->handle, &module, allocator, &copy->temporary[j]);
        hybris_scaled_free(allocator, code);
        if (result != VK_SUCCESS) {
            /* A failed creation does not return an owned shader handle. */
            copy->temporary[j] = VK_NULL_HANDLE;
            goto done;
        }
        stage->module = copy->temporary[j];
    }
    if (count) {
        copy->input = *input;
        copy->input.pVertexAttributeDescriptions = copy->attributes;
        info->pVertexInputState = &copy->input;
    }
    if (discard) {
        /* A terminating fragment shader makes the previously ignored fragment
         * stage inert even if a backend fails to clip a degenerate primitive. */
        static const uint32_t kill_fragment[] = {
            0x07230203, 0x00010000, 0, 5, 0,
            0x00020011, 1, 0x0003000e, 0, 1,
            0x0005000f, 4, 3, 0x6e69616d, 0,
            0x00030010, 3, 7, 0x00020013, 1,
            0x00030021, 2, 1, 0x00050036, 1, 3, 0, 2,
            0x000200f8, 4, 0x000100fc, 0x00010038};
        uint32_t slot = info->stageCount;
        for (uint32_t i = 0; i < info->stageCount; ++i)
            if (copy->stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) slot = i;
        if (slot == info->stageCount) { ++info->stageCount; ++copy->stage_count; }
        if (copy->temporary[slot]) device->destroy_shader(device->handle, copy->temporary[slot], allocator);
        copy->temporary[slot] = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(kill_fragment), .pCode = kill_fragment};
        result = device->create_shader(device->handle, &module, allocator, &copy->temporary[slot]);
        if (result != VK_SUCCESS) { copy->temporary[slot] = VK_NULL_HANDLE; goto done; }
        copy->stages[slot] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = copy->temporary[slot], .pName = "main"};
        result = discard_state(device, info, copy, allocator);
        if (result != VK_SUCCESS) goto done;
        fprintf(stderr, "HYBRIS_VERTEX_STORES compensated rasterizer discard stage=%u\n", last_stage);
    }
    info->pStages = copy->stages;
done:
    hybris_scaled_free(allocator, attrs);
    return result;
}
static VkResult VKAPI_CALL create_pipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    struct shader_device *device = find_device(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->mask && !device->point_size && !device->inout && !device->clip && !device->vertex_stores)
        return device->create_pipelines(handle, cache, count, infos, allocator, pipelines);
    VkGraphicsPipelineCreateInfo *changed = hybris_scaled_alloc(allocator, count * sizeof(*changed), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    struct pipeline_copy *copies = hybris_scaled_alloc(allocator, count * sizeof(*copies), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    const char *reason = NULL;
    for (uint32_t i = 0; i < count; ++i) pipelines[i] = VK_NULL_HANDLE;
    if (copies) memset(copies, 0, count * sizeof(*copies));
    if (!changed || !copies) goto done;
    memcpy(changed, infos, count * sizeof(*changed));
    for (uint32_t i = 0; i < count; ++i) {
        result = convert_pipeline(device, &changed[i], &copies[i], allocator, &reason);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "HYBRIS_SCALED_VERTEX pipeline=%u result=%d reason=%s\n", i, result, reason ? reason : "allocation or driver failure");
            goto done;
        }
    }
    result = device->create_pipelines(handle, cache, count, changed, allocator, pipelines);
done:
    if (copies) for (uint32_t i = 0; i < count; ++i) {
        if (copies[i].temporary) for (uint32_t j = 0; j < copies[i].stage_count; ++j)
            if (copies[i].temporary[j]) device->destroy_shader(handle, copies[i].temporary[j], allocator);
        hybris_scaled_free(allocator, copies[i].temporary);
        hybris_scaled_free(allocator, copies[i].stages);
        hybris_scaled_free(allocator, copies[i].attributes);
        hybris_scaled_free(allocator, copies[i].attachments);
        hybris_scaled_free(allocator, copies[i].dynamic_states);
    }
    hybris_scaled_free(allocator, copies);
    hybris_scaled_free(allocator, changed);
    return result;
}
PFN_vkVoidFunction hybris_shader_proc(const char *name)
{
    if (!hybris_shader_enabled()) return NULL;
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)create_shader;
    if (!strcmp(name, "vkDestroyShaderModule")) return (PFN_vkVoidFunction)destroy_shader;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)create_pipelines;
    return NULL;
}
