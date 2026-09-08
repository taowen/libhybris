/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "scaled_dispatch.h"
#include "scaled_vertex.h"
#include "scaled_formats.h"
#include "spirv_entry.h"
#include "shader_cleanup.h"
#include "spirv_builtins.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>


struct shader {
    VkShaderModule handle;
    size_t size;
    uint32_t *code;
    VkAllocationCallbacks allocator;
    int custom;
    struct shader *next;
};
struct scaled_device {
    VkDevice handle;
    PFN_vkCreateShaderModule create_shader;
    PFN_vkDestroyShaderModule destroy_shader;
    PFN_vkCreateGraphicsPipelines create_pipelines;
    unsigned mask;
    VkAllocationCallbacks allocator;
    int custom;
    struct shader *shaders;
    struct scaled_device *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct scaled_device *devices;
static struct scaled_device *find_device(VkDevice handle)
{
    pthread_mutex_lock(&guard);
    struct scaled_device *device = devices;
    while (device && device->handle != handle) device = device->next;
    pthread_mutex_unlock(&guard);
    return device;
}
VkResult hybris_scaled_device_create(VkDevice handle, VkPhysicalDevice physical,
    PFN_vkGetDeviceProcAddr resolver, PFN_vkGetPhysicalDeviceFormatProperties query,
    const VkAllocationCallbacks *allocator)
{
    if (!hybris_scaled_enabled()) return VK_SUCCESS;
    struct scaled_device *device = hybris_scaled_alloc(allocator, sizeof(*device), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
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
void hybris_scaled_device_destroy(VkDevice handle)
{
    pthread_mutex_lock(&guard);
    struct scaled_device **link = &devices;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    struct scaled_device *device = *link;
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
    struct scaled_device *device = find_device(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->mask) return device->create_shader(handle, info, allocator, module);
    struct shader *shader = hybris_scaled_alloc(allocator, sizeof(*shader), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!shader) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(shader, 0, sizeof(*shader));
    shader->custom = allocator != NULL;
    if (allocator) shader->allocator = *allocator;
    shader->code = hybris_scaled_alloc(allocator, info->codeSize, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!shader->code) { free_shader(shader); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    shader->size = info->codeSize;
    memcpy(shader->code, info->pCode, info->codeSize);
    VkResult result = device->create_shader(handle, info, allocator, module);
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
    struct scaled_device *device = find_device(handle);
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
};
static VkResult convert_pipeline(struct scaled_device *device, VkGraphicsPipelineCreateInfo *info,
    struct pipeline_copy *copy, const VkAllocationCallbacks *allocator, const char **reason)
{
    *reason = "unsupported pipeline state for experimental scaled conversion";
    /* These paths can ignore pVertexInputState entirely. Check them before
     * dereferencing it; dynamic input would also require command-time shader
     * variants, which this experimental pipeline-only fallback cannot supply. */
    if (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) return VK_ERROR_UNKNOWN;
    int vertex_stage = 0;
    for (uint32_t j = 0; j < info->stageCount; ++j)
        vertex_stage |= info->pStages[j].stage == VK_SHADER_STAGE_VERTEX_BIT;
    if (!vertex_stage) return VK_SUCCESS;
    if (info->pDynamicState) for (uint32_t j = 0; j < info->pDynamicState->dynamicStateCount; ++j)
        if (info->pDynamicState->pDynamicStates[j] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) return VK_ERROR_UNKNOWN;
    if (!info->pVertexInputState) return VK_SUCCESS;
    const VkPipelineVertexInputStateCreateInfo *input = info->pVertexInputState;
    unsigned count = 0;
    for (uint32_t j = 0; j < input->vertexAttributeDescriptionCount; ++j)
        for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i)
            if ((device->mask & (1u << i)) && input->pVertexAttributeDescriptions[j].format == hybris_scaled_formats[i].scaled) ++count;
    if (!count) return VK_SUCCESS;
    /* Scaled and integer fetch use the same bytes and binding cadence. Keep
     * the divisor chain (EXT/KHR aliases) intact for the backend; reject other
     * vertex-input extensions whose interaction has not been established. */
    for (const VkBaseInStructure *chain = input->pNext; chain; chain = chain->pNext)
        if (chain->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT)
            return VK_ERROR_UNKNOWN;
    struct hybris_scaled_attribute *attrs = hybris_scaled_alloc(allocator, count * sizeof(*attrs), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    copy->attributes = hybris_scaled_alloc(allocator, input->vertexAttributeDescriptionCount * sizeof(*copy->attributes), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    copy->stages = hybris_scaled_alloc(allocator, info->stageCount * sizeof(*copy->stages), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    copy->stage_count = info->stageCount;
    copy->temporary = hybris_scaled_alloc(allocator, info->stageCount * sizeof(*copy->temporary), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (copy->temporary) memset(copy->temporary, 0, info->stageCount * sizeof(*copy->temporary));
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!attrs || !copy->attributes || !copy->stages || !copy->temporary) goto done;
    memcpy(copy->attributes, input->pVertexAttributeDescriptions, input->vertexAttributeDescriptionCount * sizeof(*copy->attributes));
    memcpy(copy->stages, info->pStages, info->stageCount * sizeof(*copy->stages));
    count = 0;
    for (uint32_t j = 0; j < input->vertexAttributeDescriptionCount; ++j)
        for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i)
            if ((device->mask & (1u << i)) && copy->attributes[j].format == hybris_scaled_formats[i].scaled) {
                attrs[count++] = (struct hybris_scaled_attribute){copy->attributes[j].location, hybris_scaled_formats[i].is_signed};
                copy->attributes[j].format = hybris_scaled_formats[i].integer;
                break;
            }
    result = VK_SUCCESS;
    for (uint32_t j = 0; j < info->stageCount; ++j) {
        VkPipelineShaderStageCreateInfo *stage = &copy->stages[j];
        int vertex = stage->stage == VK_SHADER_STAGE_VERTEX_BIT;
        pthread_mutex_lock(&guard);
        struct shader *shader = device->shaders;
        while (shader && shader->handle != stage->module) shader = shader->next;
        pthread_mutex_unlock(&guard);
        /* Module destruction is externally synchronized against pipeline use. */
        if (!vertex && (!shader || !hybris_spirv_multiple(shader->code, shader->size))) continue;
        if (!shader || stage->pNext) {
            *reason = "stage conversion requires a captured module without stage extensions";
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
        uint32_t *code = NULL;
        size_t size = 0;
        if (vertex) result = hybris_scaled_spirv(shader->code, shader->size, stage->pName, attrs, count,
                                                stage->pSpecializationInfo, allocator, &code, &size, reason);
        else {
            uint32_t model = 0;
            while (model < 5 && (1u << model) != (uint32_t)stage->stage) ++model;
            result = model < 5 ? hybris_spirv_entry(shader->code, shader->size, model,
                stage->pName, allocator, &code, &size, reason) : VK_ERROR_UNKNOWN;
        }
        if (result != VK_SUCCESS) goto done;
        if (hybris_shader_cleanup_enabled()) {
            uint32_t *clean = NULL;
            size_t clean_size = 0;
            unsigned removed = 0;
            result = hybris_spirv_unused_builtins(code, size, allocator, &clean, &clean_size, &removed);
            if (result != VK_SUCCESS) { hybris_scaled_free(allocator, code); goto done; }
            if (clean) { hybris_scaled_free(allocator, code); code = clean; size = clean_size; }
        }
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
    copy->input = *input;
    copy->input.pVertexAttributeDescriptions = copy->attributes;
    info->pVertexInputState = &copy->input;
    info->pStages = copy->stages;
done:
    hybris_scaled_free(allocator, attrs);
    return result;
}
static VkResult VKAPI_CALL create_pipelines(VkDevice handle, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
    struct scaled_device *device = find_device(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    if (!device->mask) return device->create_pipelines(handle, cache, count, infos, allocator, pipelines);
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
    }
    hybris_scaled_free(allocator, copies);
    hybris_scaled_free(allocator, changed);
    return result;
}
PFN_vkVoidFunction hybris_scaled_proc(const char *name)
{
    if (!hybris_scaled_enabled()) return NULL;
    if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)create_shader;
    if (!strcmp(name, "vkDestroyShaderModule")) return (PFN_vkVoidFunction)destroy_shader;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)create_pipelines;
    return NULL;
}
