/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_command_state.h"
#include <stdlib.h>
#include <string.h>

struct hybris_bc_layout {
    VkPipelineLayout handle, clone;
    size_t references;
    struct hybris_bc_layout *next;
};
enum saved_kind { SAVED_DESCRIPTORS, SAVED_PUSH };
struct hybris_bc_saved_command {
    struct hybris_bc_saved_command *next;
    struct hybris_bc_layout *layout;
    enum saved_kind kind;
    uint32_t first, count, offset_count;
    VkShaderStageFlags stages;
    /* Descriptor handles followed by uint32_t offsets, or push bytes. */
    _Alignas(VkDescriptorSet) unsigned char data[];
};

void hybris_bc_layouts_init(struct hybris_bc_layouts *layouts, VkDevice device,
    PFN_vkGetDeviceProcAddr resolver)
{
    memset(layouts, 0, sizeof(*layouts));
    layouts->device = device;
    layouts->resolver = resolver;
    pthread_mutex_init(&layouts->guard, NULL);
}

static void release_layout(struct hybris_bc_layouts *layouts, struct hybris_bc_layout *layout)
{
    pthread_mutex_lock(&layouts->guard);
    int destroy = --layout->references == 0;
    pthread_mutex_unlock(&layouts->guard);
    if (destroy) {
        PFN_vkDestroyPipelineLayout destroy_layout =
            (PFN_vkDestroyPipelineLayout)layouts->resolver(layouts->device, "vkDestroyPipelineLayout");
        destroy_layout(layouts->device, layout->clone, NULL);
        free(layout);
    }
}

VkResult hybris_bc_layout_add(struct hybris_bc_layouts *layouts, VkPipelineLayout handle,
    const VkPipelineLayoutCreateInfo *info)
{
    struct hybris_bc_layout *layout = calloc(1, sizeof(*layout));
    if (!layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    PFN_vkCreatePipelineLayout create =
        (PFN_vkCreatePipelineLayout)layouts->resolver(layouts->device, "vkCreatePipelineLayout");
    VkResult result = create(layouts->device, info, NULL, &layout->clone);
    if (result != VK_SUCCESS) { free(layout); return result; }
    layout->handle = handle;
    layout->references = 1;
    pthread_mutex_lock(&layouts->guard);
    layout->next = layouts->head;
    layouts->head = layout;
    pthread_mutex_unlock(&layouts->guard);
    return VK_SUCCESS;
}

void hybris_bc_layout_remove(struct hybris_bc_layouts *layouts, VkPipelineLayout handle)
{
    pthread_mutex_lock(&layouts->guard);
    struct hybris_bc_layout **link = &layouts->head;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    struct hybris_bc_layout *layout = *link;
    if (layout) *link = layout->next;
    pthread_mutex_unlock(&layouts->guard);
    if (layout) release_layout(layouts, layout);
}

void hybris_bc_layouts_finish(struct hybris_bc_layouts *layouts)
{
    /* All command-buffer journals must have been retired first. */
    while (layouts->head) hybris_bc_layout_remove(layouts, layouts->head->handle);
    pthread_mutex_destroy(&layouts->guard);
}

void hybris_bc_command_state_init(struct hybris_bc_command_state *state,
    struct hybris_bc_layouts *layouts)
{
    memset(state, 0, sizeof(*state));
    state->layouts = layouts;
}

void hybris_bc_command_state_reset(struct hybris_bc_command_state *state)
{
    struct hybris_bc_saved_command *saved = state->first;
    while (saved) {
        struct hybris_bc_saved_command *next = saved->next;
        release_layout(state->layouts, saved->layout);
        free(saved);
        saved = next;
    }
    hybris_bc_command_state_init(state, state->layouts);
}

static struct hybris_bc_saved_command *save(struct hybris_bc_command_state *state,
    VkPipelineLayout handle, size_t bytes)
{
    if (state->error != VK_SUCCESS) return NULL;
    if (bytes > SIZE_MAX - sizeof(struct hybris_bc_saved_command)) {
        state->error = VK_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }
    struct hybris_bc_saved_command *saved = calloc(1, sizeof(*saved) + bytes);
    if (!saved) { state->error = VK_ERROR_OUT_OF_HOST_MEMORY; return NULL; }
    pthread_mutex_lock(&state->layouts->guard);
    struct hybris_bc_layout *layout = state->layouts->head;
    while (layout && layout->handle != handle) layout = layout->next;
    if (layout && layout->references != SIZE_MAX) {
        ++layout->references;
        saved->layout = layout;
    }
    pthread_mutex_unlock(&state->layouts->guard);
    if (!saved->layout) {
        free(saved);
        state->error = layout ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
        return NULL;
    }
    if (state->last) state->last->next = saved;
    else state->first = saved;
    state->last = saved;
    return saved;
}

void hybris_bc_save_pipeline(struct hybris_bc_command_state *state,
    VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
    if (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) state->compute_pipeline = pipeline;
}

void hybris_bc_save_descriptors(struct hybris_bc_command_state *state,
    VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set,
    uint32_t count, const VkDescriptorSet *sets, uint32_t offset_count,
    const uint32_t *offsets)
{
    if (bind_point != VK_PIPELINE_BIND_POINT_COMPUTE || !count) return;
    uint64_t bytes = (uint64_t)count * sizeof(*sets) + (uint64_t)offset_count * sizeof(*offsets);
    if (bytes > SIZE_MAX) { state->error = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
    struct hybris_bc_saved_command *saved = save(state, layout, (size_t)bytes);
    if (!saved) return;
    saved->kind = SAVED_DESCRIPTORS;
    saved->first = first_set;
    saved->count = count;
    saved->offset_count = offset_count;
    memcpy(saved->data, sets, (size_t)count * sizeof(*sets));
    if (offset_count)
        memcpy(saved->data + (size_t)count * sizeof(*sets), offsets, (size_t)offset_count * sizeof(*offsets));
}

void hybris_bc_save_push(struct hybris_bc_command_state *state, VkPipelineLayout layout,
    VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values)
{
    /* Keep all stages in order: a range may cover graphics and compute, and
     * replay must preserve later graphics writes to the same byte positions. */
    struct hybris_bc_saved_command *saved = save(state, layout, size);
    if (!saved) return;
    saved->kind = SAVED_PUSH;
    saved->stages = stages;
    saved->first = offset;
    saved->count = size;
    if (size) memcpy(saved->data, values, size);
}

void hybris_bc_restore_compute(struct hybris_bc_command_state *state, VkCommandBuffer command)
{
    if (state->error != VK_SUCCESS) return;
    PFN_vkGetDeviceProcAddr resolver = state->layouts->resolver;
    VkDevice device = state->layouts->device;
    PFN_vkCmdBindPipeline pipeline = (PFN_vkCmdBindPipeline)resolver(device, "vkCmdBindPipeline");
    PFN_vkCmdBindDescriptorSets descriptors =
        (PFN_vkCmdBindDescriptorSets)resolver(device, "vkCmdBindDescriptorSets");
    PFN_vkCmdPushConstants push = (PFN_vkCmdPushConstants)resolver(device, "vkCmdPushConstants");
    if (state->compute_pipeline) pipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state->compute_pipeline);
    for (const struct hybris_bc_saved_command *saved = state->first; saved; saved = saved->next) {
        if (saved->kind == SAVED_DESCRIPTORS) {
            descriptors(command, VK_PIPELINE_BIND_POINT_COMPUTE, saved->layout->clone,
                saved->first, saved->count, (const VkDescriptorSet *)saved->data,
                saved->offset_count,
                (const uint32_t *)(saved->data + (size_t)saved->count * sizeof(VkDescriptorSet)));
        } else {
            push(command, saved->layout->clone, saved->stages, saved->first, saved->count, saved->data);
        }
    }
}
