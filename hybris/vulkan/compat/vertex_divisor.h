/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_VERTEX_DIVISOR_H
#define HYBRIS_VERTEX_DIVISOR_H
#include "../layer/layer.h"
#define HYBRIS_DIVISOR_BINDINGS 32
struct hybris_divisor_policy { int alias, zero; };
struct hybris_divisor_pipeline {
    VkPipelineVertexInputStateCreateInfo input;
    VkPipelineVertexInputDivisorStateCreateInfoEXT divisor;
    VkVertexInputBindingDescription *bindings;
    VkVertexInputBindingDivisorDescriptionEXT *divisors;
    uint32_t mask;
    int dynamic;
};
struct hybris_divisor_recording {
    uint32_t zero, overridden, strides_known, buffers_known;
    int dynamic;
    VkBuffer buffers[HYBRIS_DIVISOR_BINDINGS];
    VkDeviceSize offsets[HYBRIS_DIVISOR_BINDINGS], sizes[HYBRIS_DIVISOR_BINDINGS];
    VkDeviceSize strides[HYBRIS_DIVISOR_BINDINGS];
};
struct hybris_divisor_policy hybris_divisor_policy(VkPhysicalDevice physical);
VkResult hybris_divisor_pipeline_prepare(VkGraphicsPipelineCreateInfo *info,
    struct hybris_divisor_pipeline *copy, const VkAllocationCallbacks *allocator);
VkResult hybris_divisor_pipeline_publish(VkDevice device, VkPipeline pipeline,
    const struct hybris_divisor_pipeline *copy, const VkAllocationCallbacks *allocator);
void hybris_divisor_pipeline_release(struct hybris_divisor_pipeline *copy, const VkAllocationCallbacks *allocator);
void hybris_divisor_pipeline_remove(VkDevice device, VkPipeline pipeline);
void hybris_divisor_device_release(VkDevice device);
int hybris_divisor_draw(VkCommandBuffer command,const struct hybris_layer_device *device,uint32_t first_instance);
void hybris_divisor_bind(VkCommandBuffer command, const struct hybris_layer_device *device, VkPipeline pipeline);
void hybris_divisor_vertices(struct hybris_divisor_recording *state, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets, const VkDeviceSize *sizes, const VkDeviceSize *strides);
const VkDeviceSize *hybris_divisor_strides(const struct hybris_divisor_recording *state,
    uint32_t first, uint32_t count, const VkDeviceSize *strides, VkDeviceSize *out);
struct hybris_divisor_recording *hybris_layer_divisor_recording(VkCommandBuffer command);
#endif
