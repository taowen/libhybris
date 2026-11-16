/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_command_state.h"
#include <stdlib.h>
#include <string.h>

struct hybris_bc_layout {
    VkPipelineLayout handle, clone;
    size_t references;
    struct hybris_bc_layout *next, *lookup_next;
};

/* The ownership list serves teardown; recording must not scan every live
 * pipeline layout for each descriptor or push-constant write. Both structures
 * are protected by layouts->guard and own the same stable layout lifetimes. */
static size_t layout_bucket(VkPipelineLayout handle)
{
    uint64_t key = (uint64_t)handle;
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    return (size_t)key & (HYBRIS_BC_LAYOUT_BUCKETS - 1);
}
enum saved_kind { SAVED_DESCRIPTORS, SAVED_PUSH, SAVED_PUSHED_DESCRIPTORS };
struct hybris_bc_saved_command {
    struct hybris_bc_saved_command *next;
    struct hybris_bc_layout *layout;
    enum saved_kind kind;
    VkPipelineBindPoint bind_point;
    uint32_t first, count, offset_count;
    VkShaderStageFlags stages;
    /* Descriptor handles followed by uint32_t offsets, or push bytes. */
    _Alignas(VkWriteDescriptorSet) unsigned char data[];
};

void hybris_bc_layouts_init(struct hybris_bc_layouts *layouts, VkDevice device,
    PFN_vkGetDeviceProcAddr resolver)
{
    memset(layouts, 0, sizeof(*layouts));
    layouts->device = device;
    layouts->resolver = resolver;
    pthread_mutex_init(&layouts->guard, NULL);
}

void hybris_bc_layout_release(struct hybris_bc_layouts *layouts, struct hybris_bc_layout *layout)
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
    size_t bucket = layout_bucket(handle);
    layout->lookup_next = layouts->index[bucket];
    layouts->index[bucket] = layout;
    pthread_mutex_unlock(&layouts->guard);
    return VK_SUCCESS;
}

void hybris_bc_layout_remove(struct hybris_bc_layouts *layouts, VkPipelineLayout handle)
{
    pthread_mutex_lock(&layouts->guard);
    struct hybris_bc_layout **link = &layouts->head;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    struct hybris_bc_layout *layout = *link;
    if (layout) {
        *link = layout->next;
        struct hybris_bc_layout **indexed = &layouts->index[layout_bucket(handle)];
        while (*indexed && *indexed != layout) indexed = &(*indexed)->lookup_next;
        if (*indexed) *indexed = layout->lookup_next;
    }
    pthread_mutex_unlock(&layouts->guard);
    if (layout) hybris_bc_layout_release(layouts, layout);
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
    unsigned track_graphics = state->track_graphics;
    struct hybris_bc_saved_command *saved = state->first;
    while (saved) {
        struct hybris_bc_saved_command *next = saved->next;
        hybris_bc_layout_release(state->layouts, saved->layout);
        free(saved);
        saved = next;
    }
    hybris_bc_command_state_init(state, state->layouts);
    state->track_graphics = track_graphics;
}
void hybris_bc_command_state_track_graphics(struct hybris_bc_command_state *state)
{ state->track_graphics = 1; }

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
    VkResult result = hybris_bc_layout_acquire(state->layouts, handle, &saved->layout);
    if (result != VK_SUCCESS) {
        free(saved);
        state->error = result;
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
    else if (state->track_graphics && bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
        state->graphics_pipeline = pipeline;
}

void hybris_bc_save_descriptors(struct hybris_bc_command_state *state,
    VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set,
    uint32_t count, const VkDescriptorSet *sets, uint32_t offset_count,
    const uint32_t *offsets)
{
    if (!count || (bind_point != VK_PIPELINE_BIND_POINT_COMPUTE &&
        !(state->track_graphics && bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS))) return;
    uint64_t bytes = (uint64_t)count * sizeof(*sets) + (uint64_t)offset_count * sizeof(*offsets);
    if (bytes > SIZE_MAX) { state->error = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
    struct hybris_bc_saved_command *saved = save(state, layout, (size_t)bytes);
    if (!saved) return;
    saved->kind = SAVED_DESCRIPTORS;
    saved->bind_point = bind_point;
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

/* The caller's write arrays and payloads cease to exist after recording.
 * Keep immutable snapshots, in the same order as ordinary descriptor binds.
 * Replaying the writes preserves partial updates to an existing pushed set. */
static int descriptor_payload(VkDescriptorType type)
{
    switch(type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return 1;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return 2;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 3;
    default: return 0;
    }
}
void hybris_bc_save_pushed_descriptors(struct hybris_bc_command_state *state,
    VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set,
    uint32_t count, const VkWriteDescriptorSet *writes)
{
    if (!count || (point != VK_PIPELINE_BIND_POINT_COMPUTE &&
        !(state->track_graphics && point == VK_PIPELINE_BIND_POINT_GRAPHICS))) return;
    size_t bytes = (size_t)count * sizeof(*writes);
    for (uint32_t i = 0; i < count; ++i) {
        const VkWriteDescriptorSet *w = &writes[i];
        int kind=descriptor_payload(w->descriptorType);
        size_t element = kind==1 ? sizeof(VkDescriptorImageInfo) :
            kind==2 ? sizeof(VkDescriptorBufferInfo) : kind==3 ? sizeof(VkBufferView) : 0;
        if (w->pNext || (!element && w->descriptorCount)) {
            state->error = VK_ERROR_FEATURE_NOT_PRESENT; return;
        }
        if (element && w->descriptorCount > (SIZE_MAX - bytes) / element) {
            state->error = VK_ERROR_OUT_OF_HOST_MEMORY; return;
        }
        bytes += (size_t)w->descriptorCount * element;
    }
    struct hybris_bc_saved_command *saved = save(state, layout, bytes);
    if (!saved) return;
    saved->kind = SAVED_PUSHED_DESCRIPTORS;
    saved->bind_point = point; saved->first = set; saved->count = count;
    VkWriteDescriptorSet *copy = (void *)saved->data;
    unsigned char *payload = (void *)(copy + count);
    for (uint32_t i = 0; i < count; ++i) {
        copy[i] = writes[i];
        int kind=descriptor_payload(writes[i].descriptorType);
        size_t element = kind==1 ? sizeof(VkDescriptorImageInfo) :
            kind==2 ? sizeof(VkDescriptorBufferInfo) : kind==3 ? sizeof(VkBufferView) : 0;
        size_t size = (size_t)writes[i].descriptorCount * element;
        const void *source = kind==1 ? (const void *)writes[i].pImageInfo :
            kind==2 ? (const void *)writes[i].pBufferInfo : (const void *)writes[i].pTexelBufferView;
        copy[i].pImageInfo = NULL; copy[i].pBufferInfo = NULL; copy[i].pTexelBufferView = NULL;
        if (kind==1) copy[i].pImageInfo = (void *)payload;
        else if (kind==2) copy[i].pBufferInfo = (void *)payload;
        else if (kind==3) copy[i].pTexelBufferView = (void *)payload;
        if (size) memcpy(payload, source, size);
        payload += size;
    }
}

static void restore(struct hybris_bc_command_state *state, VkCommandBuffer command, VkPipelineBindPoint bind_point)
{
    if (state->error != VK_SUCCESS) return;
    PFN_vkGetDeviceProcAddr resolver = state->layouts->resolver;
    VkDevice device = state->layouts->device;
    PFN_vkCmdBindPipeline pipeline = (PFN_vkCmdBindPipeline)resolver(device, "vkCmdBindPipeline");
    PFN_vkCmdBindDescriptorSets descriptors =
        (PFN_vkCmdBindDescriptorSets)resolver(device, "vkCmdBindDescriptorSets");
    PFN_vkCmdPushConstants push = (PFN_vkCmdPushConstants)resolver(device, "vkCmdPushConstants");
    PFN_vkCmdPushDescriptorSetKHR pushed = (PFN_vkCmdPushDescriptorSetKHR)resolver(device, "vkCmdPushDescriptorSetKHR");
    VkPipeline handle = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? state->compute_pipeline : state->graphics_pipeline;
    if (handle) pipeline(command, bind_point, handle);
    for (const struct hybris_bc_saved_command *saved = state->first; saved; saved = saved->next) {
        if (saved->kind == SAVED_DESCRIPTORS) {
            if (saved->bind_point != bind_point) continue;
            descriptors(command, bind_point, saved->layout->clone,
                saved->first, saved->count, (const VkDescriptorSet *)saved->data,
                saved->offset_count,
                (const uint32_t *)(saved->data + (size_t)saved->count * sizeof(VkDescriptorSet)));
        } else if (saved->kind == SAVED_PUSHED_DESCRIPTORS && saved->bind_point == bind_point) {
            pushed(command, bind_point, saved->layout->clone, saved->first,
                saved->count, (const VkWriteDescriptorSet *)saved->data);
        }
    }
    /* Preserve the complete stage mask required by each layout's ranges.
     * Restore unrelated stages first, then the stages about to execute. Some
     * backends share push storage across bind points; replaying an unrelated
     * layout last would overwrite the active pipeline's restored bytes. */
    VkShaderStageFlags active = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ?
        VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (const struct hybris_bc_saved_command *saved = state->first; saved; saved = saved->next)
            if (saved->kind == SAVED_PUSH && !!(saved->stages & active) == (int)pass)
                push(command, saved->layout->clone, saved->stages, saved->first, saved->count, saved->data);
}
void hybris_bc_restore_compute(struct hybris_bc_command_state *state, VkCommandBuffer command)
{ restore(state, command, VK_PIPELINE_BIND_POINT_COMPUTE); }
void hybris_bc_restore_graphics(struct hybris_bc_command_state *state, VkCommandBuffer command)
{ if (state->track_graphics) restore(state, command, VK_PIPELINE_BIND_POINT_GRAPHICS); }

VkResult hybris_bc_layout_acquire(struct hybris_bc_layouts *layouts, VkPipelineLayout handle,
    struct hybris_bc_layout **out)
{
    *out = NULL;
    pthread_mutex_lock(&layouts->guard);
    struct hybris_bc_layout *layout = layouts->index[layout_bucket(handle)];
    while (layout && layout->handle != handle) layout = layout->lookup_next;
    VkResult result = layout ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
    if (layout && layout->references != SIZE_MAX) {
        ++layout->references; *out = layout; result = VK_SUCCESS;
    }
    pthread_mutex_unlock(&layouts->guard);
    return result;
}
VkResult hybris_bc_visit_graphics(const struct hybris_bc_command_state *state,
    const struct hybris_bc_graphics_visitor *visitor)
{
    if (state->error != VK_SUCCESS) return state->error;
    if (!state->track_graphics || !visitor) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (const struct hybris_bc_saved_command *s = state->first; s; s = s->next) {
        VkResult result = VK_SUCCESS;
        if (s->kind == SAVED_DESCRIPTORS && s->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && visitor->descriptors)
            result = visitor->descriptors(visitor->user, s->layout, s->first, s->count,
                (const VkDescriptorSet *)s->data, s->offset_count,
                (const uint32_t *)(s->data + (size_t)s->count * sizeof(VkDescriptorSet)));
        else if (s->kind == SAVED_PUSH && (s->stages & VK_SHADER_STAGE_VERTEX_BIT) && visitor->push)
            result = visitor->push(visitor->user, s->layout, s->stages, s->first, s->count, s->data);
        else if (s->kind == SAVED_PUSHED_DESCRIPTORS && s->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}
