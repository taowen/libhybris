/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_AGGREGATE_H
#define HYBRIS_SPIRV_AGGREGATE_H
#include "scaled_vertex.h"
/* Null output on success means no aggregate needs lowering. */
VkResult hybris_spirv_aggregate(const uint32_t *code, size_t size, const char *entry,
    const struct hybris_scaled_attribute *attributes, uint32_t attribute_count,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason);
#endif
