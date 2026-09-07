#ifndef HYBRIS_ICD_WSI_H
#define HYBRIS_ICD_WSI_H

#include <vulkan/vulkan.h>
#include "hwvulkan.h"

struct hybris_vk_wayland_window;

struct hybris_icd_physical {
    VkInstance instance;
    uint64_t generation;
    PFN_vkGetInstanceProcAddr resolver;
};

VkResult hybris_icd_wsi_enumerate(hwvulkan_device_t *hal, const char *layer,
    uint32_t *count, VkExtensionProperties *properties);
VkResult hybris_icd_wsi_prepare_instance(const VkInstanceCreateInfo *info,
    VkInstanceCreateInfo *filtered, const char ***names, int *surface_enabled,
    int *wayland_enabled);
void hybris_icd_wsi_finish_instance(const char **names);
PFN_vkVoidFunction hybris_icd_wsi_proc(const char *name, int surface_enabled,
    int wayland_enabled);
void hybris_icd_wsi_release_instance(VkInstance instance);

int hybris_icd_lookup_instance_wsi(VkInstance instance, int *surface_enabled,
    int *wayland_enabled, uint64_t *generation);
int hybris_icd_lookup_physical(VkPhysicalDevice physical,
    struct hybris_icd_physical *out);
int hybris_icd_physical_has_native_buffer(VkPhysicalDevice physical);
int hybris_icd_wsi_graphics_family(VkPhysicalDevice physical, uint32_t index);
struct hybris_vk_wayland_window *hybris_icd_wsi_surface_window(VkSurfaceKHR surface,
    VkInstance *instance, uint64_t *generation);

#endif
