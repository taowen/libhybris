/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SCALED_VERTEX_H
#define HYBRIS_SCALED_VERTEX_H
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdlib.h>

struct hybris_scaled_attribute {
    uint32_t location;
    int is_signed;
    int rb_swizzle;
};

static inline void *hybris_scaled_alloc(const VkAllocationCallbacks *allocator,
                                        size_t size, VkSystemAllocationScope scope)
{
    return allocator ? allocator->pfnAllocation(allocator->pUserData, size,
                                                _Alignof(max_align_t), scope) : malloc(size);
}
static inline void hybris_scaled_free(const VkAllocationCallbacks *allocator, void *memory)
{
    if (memory) {
        if (allocator) allocator->pfnFree(allocator->pUserData, memory);
        else free(memory);
    }
}

/* Returns a new module; the caller owns it through the supplied allocator.
 * Unsupported interface/pointer forms return UNKNOWN with a diagnostic reason,
 * never a partially rewritten module. No capability is added to SPIR-V. */
VkResult hybris_scaled_spirv(const uint32_t *code, size_t size, const char *entry,
    const struct hybris_scaled_attribute *attributes, uint32_t attribute_count,
    const VkSpecializationInfo *specialization,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason);
void hybris_scaled_dump(const uint32_t *original, size_t original_size,
    const uint32_t *converted, size_t converted_size,
    const struct hybris_scaled_attribute *attributes, uint32_t count,
    const VkSpecializationInfo *specialization);
#endif
