/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>
int hybris_mali_spec_enabled(void);
int hybris_mali_spec_active(VkPhysicalDevice physical);
/* In-place only on a private copy. Ordinary constant composites stay unchanged. */
VkResult hybris_spirv_mali_spec_constants(uint32_t *code,size_t bytes,
    const VkAllocationCallbacks *allocator,unsigned *changed);
