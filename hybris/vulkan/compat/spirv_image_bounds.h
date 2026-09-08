/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_IMAGE_BOUNDS_H
#define HYBRIS_SPIRV_IMAGE_BOUNDS_H
#include "scaled_vertex.h"
int hybris_image_bounds_enabled(void);
/* After OpSampledImage / OpImageFetch, if OpSelect's false object is a zero
 * constant, replace it with the true object. Null output means no change. */
VkResult hybris_spirv_image_bounds(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *rewritten);
#endif
