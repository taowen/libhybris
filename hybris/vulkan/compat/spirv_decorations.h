/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_DECORATIONS_H
#define HYBRIS_SPIRV_DECORATIONS_H
#include "scaled_vertex.h"
/* Expand group applications; null output means no applications were present. */
VkResult hybris_spirv_decorations(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason);
#endif
