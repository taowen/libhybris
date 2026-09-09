/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_CLIP_DISTANCE_H
#define HYBRIS_CLIP_DISTANCE_H
#include "scaled_vertex.h"
int hybris_clip_enabled(void);
int hybris_clip_active(VkPhysicalDevice physical);
int hybris_clip_pipeline(const VkGraphicsPipelineCreateInfo *info);
void hybris_clip_filter_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *features);
PFN_vkVoidFunction hybris_clip_policy_proc(const char *name);
/* count==0 means the vertex module does not write ClipDistance. */
VkResult hybris_spirv_clip_plan(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size,
    uint32_t *location, uint32_t *count);
VkResult hybris_spirv_clip_vertex(const uint32_t *code, size_t size, uint32_t location,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *rewritten);
VkResult hybris_spirv_clip_fragment(const uint32_t *code, size_t size, uint32_t location, uint32_t count,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *rewritten);
#endif
