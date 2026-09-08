/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_POINT_SIZE_H
#define HYBRIS_POINT_SIZE_H
#include "scaled_vertex.h"
int hybris_point_size_enabled(void);
int hybris_point_size_pipeline(const VkGraphicsPipelineCreateInfo *info);
VkResult hybris_spirv_point_size(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *removed);
#endif
