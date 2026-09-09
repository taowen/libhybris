/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_MEMORY_VISIBILITY_H
#define HYBRIS_MEMORY_VISIBILITY_H
#include <vulkan/vulkan.h>
PFN_vkVoidFunction hybris_memory_visibility_proc(const char *name);
void hybris_memory_visibility_release_device(VkDevice device);
#endif
