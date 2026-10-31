/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_CLIP_DISTANCE_H
#define HYBRIS_CLIP_DISTANCE_H
#include "scaled_vertex.h"
int hybris_clip_enabled(void);
int hybris_clip_active(VkPhysicalDevice physical);
int hybris_cull_active(VkPhysicalDevice physical);
int hybris_stage_point_active(VkPhysicalDevice physical);
PFN_vkVoidFunction hybris_clip_policy_proc(const char *name);
#endif
