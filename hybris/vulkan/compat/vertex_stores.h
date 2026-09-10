/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_VERTEX_STORES_H
#define HYBRIS_VERTEX_STORES_H
#include "scaled_vertex.h"
int hybris_vertex_stores_active(VkPhysicalDevice physical);
struct hybris_vertex_rendering { uint32_t colors; VkSampleCountFlagBits samples; };
VkResult hybris_vertex_rendering_info(VkDevice device,
    const VkGraphicsPipelineCreateInfo *info, struct hybris_vertex_rendering *out);
PFN_vkVoidFunction hybris_vertex_stores_proc(const char *name);
void hybris_vertex_stores_release_device(VkDevice device);
VkResult hybris_spirv_storage_writes(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, int *writes);
/* Preserve execution and memory side effects, replacing only the final
 * pre-rasterization Position immediately before return/EmitVertex. */
VkResult hybris_spirv_discard(const uint32_t *code, size_t size, uint32_t model,
    const char *entry, const VkAllocationCallbacks *allocator, uint32_t **output,
    size_t *output_size, const char **reason);
#endif
