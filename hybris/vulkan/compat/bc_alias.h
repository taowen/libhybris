/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_ALIAS_H
#define HYBRIS_BC_ALIAS_H
#include "bc_image.h"
VkResult hybris_bc_alias_prepare(struct hybris_bc_image *, const VkImageCreateInfo *,
    const VkMemoryRequirements *, uint32_t memory_type, VkBool32 dedicated);
VkResult hybris_bc_alias_bind(struct hybris_bc_image *, VkDeviceMemory, VkDeviceSize);
void hybris_bc_alias_release(struct hybris_bc_image *);
void hybris_bc_alias_forget(VkDevice, VkDeviceMemory);
#endif
