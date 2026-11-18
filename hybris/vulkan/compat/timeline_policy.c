/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "malibridge/timeline.h"
#include "../layer/layer.h"
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

int hybris_timeline_enabled(void)
{
    if (getauxval(AT_SECURE)) return 0;
    const char *setting = getenv("HYBRIS_TIMELINE_SEMAPHORE");
    return !setting || strcmp(setting, "0");
}

int hybris_timeline_emulated(VkPhysicalDevice physical)
{
    struct hybris_layer_physical owner;
    if (!hybris_timeline_enabled() || !hybris_layer_lookup_physical(physical, &owner)) return 0;
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)owner.resolver(owner.instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures2 features =
        (PFN_vkGetPhysicalDeviceFeatures2)owner.resolver(owner.instance, "vkGetPhysicalDeviceFeatures2");
    if (!features)
        features = (PFN_vkGetPhysicalDeviceFeatures2)owner.resolver(owner.instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!properties || !features) return 0;
    VkPhysicalDeviceProperties api = {0};
    properties(physical, &api);
    /* The initial compatibility path is intentionally scoped to Vulkan 1.1. */
    if (api.apiVersion < VK_API_VERSION_1_1 || api.apiVersion >= VK_API_VERSION_1_2) return 0;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline};
    features(physical, &supported);
    return !timeline.timelineSemaphore;
}

int hybris_timeline_requested(const VkDeviceCreateInfo *info)
{
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            return ((const VkPhysicalDeviceTimelineSemaphoreFeatures *)node)->timelineSemaphore;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            return ((const VkPhysicalDeviceVulkan12Features *)node)->timelineSemaphore;
    }
    return 0;
}

void hybris_timeline_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES &&
            hybris_timeline_emulated(physical))
            ((VkPhysicalDeviceTimelineSemaphoreFeatures *)node)->timelineSemaphore = VK_TRUE;
}

void hybris_timeline_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *out)
{
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES &&
            hybris_timeline_emulated(physical))
            ((VkPhysicalDeviceTimelineSemaphoreProperties *)node)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
}
