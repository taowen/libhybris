#ifndef HYBRIS_ICD_SWAPCHAIN_H
#define HYBRIS_ICD_SWAPCHAIN_H
#include <vulkan/vulkan.h>

VkResult hybris_icd_prepare_device(VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr resolver,
    VkInstance instance, const VkDeviceCreateInfo *info, VkDeviceCreateInfo *filtered,
    const char ***names, int *swapchain_enabled);
void hybris_icd_finish_device(const char **names);
void hybris_icd_swapchain_release_device(VkDevice device);
PFN_vkVoidFunction hybris_icd_swapchain_image_proc(const char *name);
PFN_vkVoidFunction hybris_icd_swapchain_proc(const char *name, int swapchain_enabled);
VkResult hybris_icd_enumerate_device_extensions(VkPhysicalDevice physical, const char *layer,
    uint32_t *count, VkExtensionProperties *properties);

#endif
