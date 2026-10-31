/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_DISTANCE_LOWERING_H
#define HYBRIS_DISTANCE_LOWERING_H
#include "scaled_vertex.h"
struct hybris_distance_stage {
    VkShaderStageFlagBits stage;
    const char *entry;
    const uint32_t *code;
    size_t size;
};
struct hybris_distance_code { uint32_t *code; size_t size; };
/* Masks use MaliBridge VKB_SPV_CLIP/CULL/POINT_SIZE bit values. */
VkResult hybris_distance_module(unsigned what, int wrap, const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, struct hybris_distance_code *out, const char **reason);
VkResult hybris_distance_pipeline(unsigned what, int wrap, unsigned count,
    const struct hybris_distance_stage *stages, const VkAllocationCallbacks *allocator,
    struct hybris_distance_code *out, const char **reason);
#endif
