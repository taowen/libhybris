#ifndef HYBRIS_ICD_INSTANCE_H
#define HYBRIS_ICD_INSTANCE_H

#include <vulkan/vulkan.h>
#include "hwvulkan.h"

VkResult hybris_icd_create_instance(hwvulkan_device_t *hal,
    const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkInstance *instance);
unsigned hybris_icd_application_policy(VkPhysicalDevice physical);
PFN_vkVoidFunction hybris_icd_instance_proc(VkInstance instance, const char *name);

#endif
