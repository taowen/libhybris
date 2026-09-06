/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_RENDER_DISPATCH_H
#define HYBRIS_RENDER_DISPATCH_H
#include <vulkan/vulkan.h>
__attribute__((visibility("hidden")))
void hybris_render_dispatch_init(PFN_vkCreateDevice create, PFN_vkGetDeviceProcAddr resolve);
__attribute__((visibility("hidden")))
PFN_vkVoidFunction hybris_render_dispatch_proc(const char *name);
#endif
