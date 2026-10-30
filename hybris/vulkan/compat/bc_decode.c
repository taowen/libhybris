/* SPDX-License-Identifier: GPL-3.0-or-later */
#define VK_NO_PROTOTYPES
#include "bc_decode.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include "shaders/bc_decode.inc"

#define PROC(name) PFN_vk##name name = (PFN_vk##name)d->resolver(d->device, "vk" #name)
struct kernel { const uint32_t *code; size_t size; };
#define K(name) {hybris_bc_##name##_spv, sizeof(hybris_bc_##name##_spv)}
static const struct kernel kernels[] = {
    K(bc1), K(bc2), K(bc3), K(bc4), K(bc5), K(bc6h), K(bc7),
    K(rgb8), K(rgb16), K(tx_bc1), K(tx_bc2), K(tx_bc3), K(tx_bc4), K(tx_bc5), K(tx_bc7),
    K(bc1_packed), K(tx_bc6h)
};
#undef K
static int family(VkFormat f)
{
    if (f < VK_FORMAT_BC1_RGB_UNORM_BLOCK || f > VK_FORMAT_BC7_SRGB_BLOCK) return -1;
    unsigned offset = f - VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    return offset < 4 ? 0 : (int)(offset / 2 - 1);
}
static int variant(const struct hybris_bc_region *r)
{
    int f = family(r->format);
    if (f < 0) return -1;
    switch (r->output_format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        return f == 0 ? 9 : -1;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        return f == 1 ? 10 : f == 2 ? 11 : f == 6 ? 14 : -1;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        return f == 3 ? 12 : -1;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        return f == 4 ? 13 : -1;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        return f == 0 ? 15 : -1;
    case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK: return f == 5 ? 16 : -1;
    default: return r->rgb8 ? (f == 0 ? 7 : -1) : r->rgb16 ? (f == 5 ? 8 : -1) : f;
    }
}
uint32_t hybris_bc_output_block_bytes(const struct hybris_bc_region *r)
{
    int v = variant(r);
    if (v < 0) return 0;
    static const uint32_t bytes[] = {64,64,64,16,32,128,64,48,96,8,16,16,8,16,16,32,16};
    return bytes[v];
}
void hybris_bc_decoder_destroy(struct hybris_bc_decoder *d, const VkAllocationCallbacks *a)
{
    if (!d || !d->device) return;
    PROC(DestroyPipeline); PROC(DestroyPipelineLayout); PROC(DestroyDescriptorSetLayout);
    a = d->custom_allocator ? &d->allocator : NULL;
    for (unsigned q = 0; q < 3; ++q)
        for (unsigned i = 0; i < HYBRIS_BC_KERNEL_COUNT; i++)
            if (d->pipelines[q][i]) DestroyPipeline(d->device, d->pipelines[q][i], a);
    if (d->layout) DestroyPipelineLayout(d->device, d->layout, a);
    if (d->descriptor_layout) DestroyDescriptorSetLayout(d->device, d->descriptor_layout, a);
    pthread_mutex_destroy(&d->guard);
    memset(d, 0, sizeof(*d));
}
VkResult hybris_bc_decoder_create(VkDevice device, PFN_vkGetDeviceProcAddr resolver,
    const VkAllocationCallbacks *a, struct hybris_bc_decoder *d)
{
    if (!d || !device || !resolver) return VK_ERROR_INITIALIZATION_FAILED;
    memset(d, 0, sizeof(*d));
    d->device = device; d->resolver = resolver;
    d->custom_allocator = a != NULL;
    if (a) d->allocator = *a;
    if (pthread_mutex_init(&d->guard, NULL)) { memset(d, 0, sizeof(*d)); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    PROC(CreateDescriptorSetLayout); PROC(CreatePipelineLayout);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo sets = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2, .pBindings=bindings};
    VkResult result = CreateDescriptorSetLayout(device, &sets, a, &d->descriptor_layout);
    if (result != VK_SUCCESS) goto fail;
    VkPushConstantRange push = {.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT, .size=sizeof(struct hybris_bc_push)};
    VkPipelineLayoutCreateInfo layout = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1, .pSetLayouts=&d->descriptor_layout, .pushConstantRangeCount=1, .pPushConstantRanges=&push};
    result = CreatePipelineLayout(device, &layout, a, &d->layout);
    if (result == VK_SUCCESS) return result;
fail:
    hybris_bc_decoder_destroy(d, a);
    return result;
}
static VkResult pipeline(struct hybris_bc_decoder *d, int v, uint32_t quality, VkPipeline *out)
{
    pthread_mutex_lock(&d->guard);
    VkResult result = VK_SUCCESS;
    if (!d->pipelines[quality][v]) {
        PROC(CreateShaderModule); PROC(DestroyShaderModule); PROC(CreateComputePipelines);
        const VkAllocationCallbacks *a = d->custom_allocator ? &d->allocator : NULL;
        VkShaderModuleCreateInfo info = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=kernels[v].size, .pCode=kernels[v].code};
        VkShaderModule shader = VK_NULL_HANDLE;
        result = CreateShaderModule(d->device, &info, a, &shader);
        if (result == VK_SUCCESS) {
            VkSpecializationMapEntry entry = {.constantID = 0, .size = sizeof(quality)};
            VkSpecializationInfo specialization = {.mapEntryCount = 1, .pMapEntries = &entry,
                .dataSize = sizeof(quality), .pData = &quality};
            VkComputePipelineCreateInfo create = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage=VK_SHADER_STAGE_COMPUTE_BIT, .module=shader, .pName="main",
                    .pSpecializationInfo = v >= 9 && v != 15 ? &specialization : NULL}, .layout=d->layout};
            result = CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &create, a, &d->pipelines[quality][v]);
            DestroyShaderModule(d->device, shader, a);
            const char *trace = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_TRACE");
            if (trace && !strcmp(trace, "1"))
                fprintf(stderr, "HYBRIS_BC_KERNEL variant=%d result=%d\n", v, result);
        }
    }
    *out = d->pipelines[quality][v];
    pthread_mutex_unlock(&d->guard);
    return result;
}
VkResult hybris_bc_prepare(const struct hybris_bc_region *r,
    const VkPhysicalDeviceLimits *limits, struct hybris_bc_push *push)
{
    if (!r || !limits || !push) return VK_ERROR_INITIALIZATION_FAILED;
    int f = family(r->format);
    if ((r->rgb8 && (r->rgb16 || (r->format != VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
        r->format != VK_FORMAT_BC1_RGB_SRGB_BLOCK))) || (r->rgb16 && f != 5))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t output_bytes = hybris_bc_output_block_bytes(r);
    if (f < 0 || !output_bytes) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!r->width || !r->height || !r->layers || (r->source_offset & 3) || (r->destination_offset & 3) ||
        r->source_range > limits->maxStorageBufferRange || r->destination_range > limits->maxStorageBufferRange ||
        limits->maxComputeWorkGroupSize[0] < 8 || limits->maxComputeWorkGroupSize[1] < 8 ||
        limits->maxComputeWorkGroupInvocations < 64) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t row = r->row_length ? r->row_length : r->width;
    uint64_t height = r->image_height ? r->image_height : r->height;
    if (row < r->width || height < r->height || (r->row_length && (row & 3)) ||
        (r->image_height && (height & 3))) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t nx = ((uint64_t)r->width + 3) / 4, ny = ((uint64_t)r->height + 3) / 4;
    uint64_t row_blocks = (row + 3) / 4, layer_blocks = row_blocks * ((height + 3) / 4);
    if (layer_blocks > UINT32_MAX / r->layers || (nx + 7) / 8 > limits->maxComputeWorkGroupCount[0] ||
        (ny + 7) / 8 > limits->maxComputeWorkGroupCount[1] || r->layers > limits->maxComputeWorkGroupCount[2])
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t input_bytes = f == 0 || f == 3 ? 8 : 16;
    uint64_t source_bytes = (layer_blocks * (r->layers - 1) + (ny - 1) * row_blocks + nx) * input_bytes;
    uint64_t destination_bytes = nx * ny * r->layers * output_bytes;
    if (r->source_offset > r->source_range || source_bytes > r->source_range - r->source_offset ||
        r->destination_offset > r->destination_range || destination_bytes > r->destination_range - r->destination_offset ||
        r->source_offset / 4 + source_bytes / 4 > UINT32_MAX ||
        r->destination_offset / 4 + destination_bytes / 4 > UINT32_MAX) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t flags = r->format == VK_FORMAT_BC1_RGB_UNORM_BLOCK || r->format == VK_FORMAT_BC1_RGB_SRGB_BLOCK ? 1 : 0;
    if (r->format == VK_FORMAT_BC4_SNORM_BLOCK || r->format == VK_FORMAT_BC5_SNORM_BLOCK ||
        r->format == VK_FORMAT_BC6H_SFLOAT_BLOCK) flags |= 2;
    if (r->output_format == VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK || r->output_format == VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK) flags |= 4;
    *push = (struct hybris_bc_push){.source_word=(uint32_t)(r->source_offset / 4),
        .row_blocks=(uint32_t)row_blocks, .layer_blocks=(uint32_t)layer_blocks, .flags=flags,
        .destination_word=(uint32_t)(r->destination_offset / 4), .width=r->width, .height=r->height};
    return VK_SUCCESS;
}
VkResult hybris_bc_decode_record(struct hybris_bc_decoder *d,
    VkCommandBuffer command, VkDescriptorSet descriptors,
    const struct hybris_bc_region *r, const VkPhysicalDeviceLimits *limits)
{
    if (!d || !d->device || !command || !descriptors) return VK_ERROR_INITIALIZATION_FAILED;
    struct hybris_bc_push push;
    VkResult result = hybris_bc_prepare(r, limits, &push);
    if (result != VK_SUCCESS) return result;
    VkPipeline selected;
    int v = variant(r);
    if (r->quality > 2) return VK_ERROR_INITIALIZATION_FAILED;
    result = pipeline(d, v, v >= 9 && v != 15 ? r->quality : 1, &selected);
    if (result != VK_SUCCESS) return result;
    PROC(CmdBindPipeline); PROC(CmdBindDescriptorSets); PROC(CmdPushConstants); PROC(CmdDispatch);
    CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, selected);
    CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, d->layout, 0, 1, &descriptors, 0, NULL);
    CmdPushConstants(command, d->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    CmdDispatch(command, (uint32_t)(((uint64_t)r->width + 31) / 32),
        (uint32_t)(((uint64_t)r->height + 31) / 32), r->layers);
    return VK_SUCCESS;
}
