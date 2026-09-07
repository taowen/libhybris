#ifndef HYBRIS_WSI_SURFACE_LIFECYCLE_H
#define HYBRIS_WSI_SURFACE_LIFECYCLE_H
#include <wayland-client.h>
#include <vulkan/vulkan.h>
int surface_lifecycle(struct wl_display *display, struct wl_compositor *compositor,
                      VkInstance instance, PFN_vkCreateWaylandSurfaceKHR create,
                      PFN_vkDestroySurfaceKHR destroy);
int surface_extension_checks(PFN_vkGetInstanceProcAddr gip);
int surface_presentation_checks(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface, struct wl_display *display);
#endif
