/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SPIRV_ENTRY_H
#define HYBRIS_SPIRV_ENTRY_H
#include "scaled_vertex.h"
VkResult hybris_spirv_entry(const uint32_t *code, size_t size, uint32_t model,
    const char *entry, const VkAllocationCallbacks *allocator, uint32_t **output,
    size_t *output_size, const char **reason);
int hybris_spirv_literal_word(uint32_t opcode, uint32_t word);
int hybris_spirv_multiple(const uint32_t *code, size_t size);
#endif
