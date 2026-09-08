/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_INOUT_H
#define HYBRIS_SPIRV_INOUT_H
#include "scaled_vertex.h"
int hybris_inout_enabled(void);
int hybris_inout_pipeline(const VkGraphicsPipelineCreateInfo *info);
/* Widen fragment inputs that share a Location with a wider vertex output.
 * Null output means the fragment module is already compatible. Unsupported
 * forms leave the original module (SUCCESS, output NULL). */
VkResult hybris_spirv_inout(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *widened);
#endif
