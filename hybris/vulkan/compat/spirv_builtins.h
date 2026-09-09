/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_BUILTINS_H
#define HYBRIS_SPIRV_BUILTINS_H
#include "scaled_vertex.h"
/* Remove unaccessed direct/struct output clip/cull declarations. Active or
 * ambiguous uses are retained. Null output means the original is unchanged. */
VkResult hybris_spirv_unused_builtins(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *removed);
/* Automatic module preparation preserves all interface declarations. Only
 * ClipDistance/CullDistance capabilities proven unused by both input and
 * output accesses are removed, including per-vertex input arrays. */
VkResult hybris_spirv_unused_clip_capabilities(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *removed);
/* Internal pipeline-only step, after removable PointSize stores are erased. */
VkResult hybris_spirv_unused_point_size(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *removed);
#endif
