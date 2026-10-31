/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../layer/layer.h"
VkResult hybris_null_attach(const struct hybris_layer_device *owner,
    PFN_vkVoidFunction (*base)(const struct hybris_layer_device *,const char *),
    const VkDeviceCreateInfo *requested);
void hybris_null_release(VkDevice device);
PFN_vkVoidFunction hybris_null_dispatch(const char *name);
PFN_vkVoidFunction hybris_null_proc(VkDevice device,const char *name);
