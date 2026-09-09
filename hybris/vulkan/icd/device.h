#ifndef HYBRIS_ICD_DEVICE_H
#define HYBRIS_ICD_DEVICE_H
#include <vulkan/vulkan.h>

struct hybris_icd_device {
    VkDevice handle;
    uint64_t generation, instance_generation;
    PFN_vkGetDeviceProcAddr resolver;
    VkPhysicalDevice physical;
    int swapchain_enabled;
};

VkResult hybris_icd_create_device(PFN_vkCreateDevice create, PFN_vkGetDeviceProcAddr resolver,
    uint64_t instance_generation, VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDevice *device);
PFN_vkVoidFunction VKAPI_CALL hybris_icd_device_proc(VkDevice device, const char *name);
void VKAPI_CALL hybris_icd_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator);
int hybris_icd_lookup_queue(VkQueue queue, struct hybris_icd_device *out);
int hybris_icd_lookup_device(VkDevice device, struct hybris_icd_device *out);
#endif
