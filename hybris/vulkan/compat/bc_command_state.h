/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_COMMAND_STATE_H
#define HYBRIS_BC_COMMAND_STATE_H
#include <vulkan/vulkan.h>
#include <pthread.h>

struct hybris_bc_layout;
struct hybris_bc_saved_command;
/* Owned by one emulated device; initialize before publishing that device. */
struct hybris_bc_layouts {
    VkDevice device;
    PFN_vkGetDeviceProcAddr resolver;
    pthread_mutex_t guard;
    struct hybris_bc_layout *head;
};
/* Owned by one command buffer, externally synchronized with its recording.
 * Reset/free only after Vulkan permits retiring the recorded commands. */
struct hybris_bc_command_state {
    struct hybris_bc_layouts *layouts;
    VkPipeline compute_pipeline;
    struct hybris_bc_saved_command *first, *last;
    VkResult error;
};
void hybris_bc_layouts_init(struct hybris_bc_layouts *layouts, VkDevice device,
    PFN_vkGetDeviceProcAddr resolver);
void hybris_bc_layouts_finish(struct hybris_bc_layouts *layouts);
/* Call immediately after creating an application pipeline layout, while its
 * descriptor set layouts are still alive. Failure must fail that creation. */
VkResult hybris_bc_layout_add(struct hybris_bc_layouts *layouts, VkPipelineLayout handle,
    const VkPipelineLayoutCreateInfo *info);
void hybris_bc_layout_remove(struct hybris_bc_layouts *layouts, VkPipelineLayout handle);
void hybris_bc_command_state_init(struct hybris_bc_command_state *state,
    struct hybris_bc_layouts *layouts);
void hybris_bc_command_state_reset(struct hybris_bc_command_state *state);
void hybris_bc_save_pipeline(struct hybris_bc_command_state *state,
    VkPipelineBindPoint bind_point, VkPipeline pipeline);
void hybris_bc_save_descriptors(struct hybris_bc_command_state *state,
    VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set,
    uint32_t count, const VkDescriptorSet *sets, uint32_t offset_count,
    const uint32_t *offsets);
void hybris_bc_save_push(struct hybris_bc_command_state *state, VkPipelineLayout layout,
    VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *values);
/* No allocation and no ownership changes while restoring. The caller must
 * return state->error from EndCommandBuffer if a void recording hook failed. */
void hybris_bc_restore_compute(struct hybris_bc_command_state *state, VkCommandBuffer command);
#endif
