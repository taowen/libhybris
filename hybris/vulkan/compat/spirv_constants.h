/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_CONSTANTS_H
#define HYBRIS_SPIRV_CONSTANTS_H
#include "scaled_vertex.h"
struct hybris_spirv_constants;
VkResult hybris_spirv_constants_create(const uint32_t *code, size_t size,
    const VkSpecializationInfo *specialization, const VkAllocationCallbacks *allocator,
    struct hybris_spirv_constants **output);
/* Resolve a scalar 32-bit integer expression; unsupported expressions fail. */
int hybris_spirv_constant_u32(struct hybris_spirv_constants *constants,
    uint32_t id, uint32_t *value, int *is_signed);
void hybris_spirv_constants_destroy(struct hybris_spirv_constants *constants,
    const VkAllocationCallbacks *allocator);
#endif
