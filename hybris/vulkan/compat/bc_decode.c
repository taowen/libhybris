/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_decode.h"
#include <string.h>
#include "shaders/bc_decode.inc"

#define PROC(name) PFN_vk##name name = (PFN_vk##name)d->resolver(d->device, "vk" #name)

void hybris_bc_decoder_destroy(struct hybris_bc_decoder *d, const VkAllocationCallbacks *a)
{
    if (!d || !d->resolver) return;
    PROC(DestroyPipeline); PROC(DestroyPipelineLayout); PROC(DestroyDescriptorSetLayout);
    if (d->pipeline) DestroyPipeline(d->device, d->pipeline, a);
    if (d->layout) DestroyPipelineLayout(d->device, d->layout, a);
    if (d->descriptor_layout) DestroyDescriptorSetLayout(d->device, d->descriptor_layout, a);
    memset(d, 0, sizeof(*d));
}

VkResult hybris_bc_decoder_create(VkDevice device, PFN_vkGetDeviceProcAddr resolver,
    const VkAllocationCallbacks *a, struct hybris_bc_decoder *d)
{
    if (!d || !device || !resolver) return VK_ERROR_INITIALIZATION_FAILED;
    memset(d, 0, sizeof(*d));
    d->device = device;
    d->resolver = resolver;
    PROC(CreateDescriptorSetLayout); PROC(CreatePipelineLayout); PROC(CreateShaderModule);
    PROC(CreateComputePipelines); PROC(DestroyShaderModule);
    PROC(DestroyPipeline); PROC(DestroyPipelineLayout); PROC(DestroyDescriptorSetLayout);
    if (!CreateDescriptorSetLayout || !CreatePipelineLayout || !CreateShaderModule ||
        !CreateComputePipelines || !DestroyShaderModule || !DestroyPipeline ||
        !DestroyPipelineLayout || !DestroyDescriptorSetLayout) {
        memset(d, 0, sizeof(*d));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkDescriptorSetLayoutBinding bindings[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo sets = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkResult result = CreateDescriptorSetLayout(device, &sets, a, &d->descriptor_layout);
    if (result != VK_SUCCESS) goto fail;
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(struct hybris_bc_push)};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &d->descriptor_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    result = CreatePipelineLayout(device, &layout, a, &d->layout);
    if (result != VK_SUCCESS) goto fail;
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(hybris_bc_decode_spv), .pCode = hybris_bc_decode_spv};
    VkShaderModule module = VK_NULL_HANDLE;
    result = CreateShaderModule(device, &shader, a, &module);
    if (result != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = d->layout};
    result = CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, a, &d->pipeline);
    DestroyShaderModule(device, module, a);
    if (result == VK_SUCCESS) return result;
fail:
    hybris_bc_decoder_destroy(d, a);
    return result;
}

VkResult hybris_bc_prepare(const struct hybris_bc_region *r,
    const VkPhysicalDeviceLimits *limits, struct hybris_bc_push *push)
{
    if (!r || !limits || !push) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t mode;
    switch (r->format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: mode = 0; break;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: mode = 1; break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: mode = 2; break;
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: mode = 3; break;
    case VK_FORMAT_BC4_UNORM_BLOCK: mode = 4; break;
    case VK_FORMAT_BC4_SNORM_BLOCK: mode = 5; break;
    case VK_FORMAT_BC5_UNORM_BLOCK: mode = 6; break;
    case VK_FORMAT_BC5_SNORM_BLOCK: mode = 7; break;
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: mode = 9; break;
    default: return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (r->rgb8) {
        if (mode != 0) return VK_ERROR_INITIALIZATION_FAILED;
        mode = 8;
    }
    if (!r->width || !r->height || !r->layers || (r->source_offset & 3) ||
        (r->destination_offset & 3) || !limits->maxComputeWorkGroupCount[0] ||
        limits->maxComputeWorkGroupSize[0] < 64 || limits->maxComputeWorkGroupInvocations < 64 ||
        r->source_range > limits->maxStorageBufferRange ||
        r->destination_range > limits->maxStorageBufferRange)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t row = r->row_length ? r->row_length : r->width;
    uint64_t height = r->image_height ? r->image_height : r->height;
    if (row < r->width || height < r->height ||
        (r->row_length && (row & 3)) || (r->image_height && (height & 3)))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t row_blocks = (row + 3) / 4, layer_blocks = row_blocks * ((height + 3) / 4);
    uint64_t pixels_per_layer = (uint64_t)r->width * r->height;
    /* Early bounds make all subsequent products/sums fit uint64_t and all
     * shader word/pixel arithmetic fit uint32_t. */
    if (pixels_per_layer > UINT32_MAX || layer_blocks > UINT32_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t pixels = pixels_per_layer * r->layers;
    uint64_t last_layer = layer_blocks * (r->layers - 1);
    if (pixels > UINT32_MAX || last_layer > UINT32_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t blocks = last_layer + ((r->height - 1) / 4) * row_blocks + (r->width + UINT64_C(3)) / 4;
    uint64_t source_bytes = blocks * (mode < 2 || mode == 4 || mode == 5 || mode == 8 ? 8 : 16);
    uint64_t destination_bytes = mode == 8 ? ((pixels * 3 + 3) / 4) * 4 :
        mode == 4 || mode == 5 ? ((pixels + 1) / 2) * 4 : pixels * 4;
    if (r->source_offset > r->source_range || source_bytes > r->source_range - r->source_offset ||
        r->destination_offset > r->destination_range || destination_bytes > r->destination_range - r->destination_offset)
        return VK_ERROR_INITIALIZATION_FAILED;
    *push = (struct hybris_bc_push){.mode = mode, .source_word = (uint32_t)(r->source_offset / 4),
        .row_blocks = (uint32_t)row_blocks, .layer_blocks = (uint32_t)layer_blocks,
        .width = r->width, .height = r->height, .pixels = (uint32_t)pixels,
        .destination_word = (uint32_t)(r->destination_offset / 4)};
    return VK_SUCCESS;
}

VkResult hybris_bc_decode_record(const struct hybris_bc_decoder *d,
    VkCommandBuffer command, VkDescriptorSet descriptors,
    const struct hybris_bc_region *r, const VkPhysicalDeviceLimits *limits)
{
    if (!d || !d->pipeline || !command || !descriptors) return VK_ERROR_INITIALIZATION_FAILED;
    struct hybris_bc_push push;
    VkResult result = hybris_bc_prepare(r, limits, &push);
    if (result != VK_SUCCESS) return result;
    PROC(CmdBindPipeline); PROC(CmdBindDescriptorSets); PROC(CmdPushConstants); PROC(CmdDispatch);
    if (!CmdBindPipeline || !CmdBindDescriptorSets || !CmdPushConstants || !CmdDispatch)
        return VK_ERROR_INITIALIZATION_FAILED;
    CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
    CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, d->layout, 0, 1, &descriptors, 0, NULL);
    uint64_t capacity = (uint64_t)limits->maxComputeWorkGroupCount[0] * 64;
    /* Prevent the rounded dispatch size from overflowing shader invocation IDs. */
    if (capacity > UINT32_MAX - 63u) capacity = UINT32_MAX - 63u;
    while (push.first_pixel < push.pixels) {
        uint32_t remaining = push.pixels - push.first_pixel;
        push.chunk_pixels = remaining < capacity ? remaining : (uint32_t)capacity;
        CmdPushConstants(command, d->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        CmdDispatch(command, (push.chunk_pixels + 63) / 64, 1, 1);
        push.first_pixel += push.chunk_pixels;
    }
    return VK_SUCCESS;
}
