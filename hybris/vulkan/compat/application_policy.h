/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_APPLICATION_POLICY_H
#define HYBRIS_APPLICATION_POLICY_H
#include <vulkan/vulkan.h>
enum {
    HYBRIS_APP_HOST_UPLOAD_FLUSH = 1u << 0,
    HYBRIS_APP_RENDERING_SEGMENTS = 1u << 1,
    HYBRIS_APP_HOST_READBACK_INVALIDATE = 1u << 2,
};
unsigned hybris_application_policy(const VkApplicationInfo *application);
unsigned hybris_application_device_policy(unsigned policy, const VkDeviceCreateInfo *info);
#endif
