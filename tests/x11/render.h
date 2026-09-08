/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_X11_PROBE_RENDER_H
#define HYBRIS_X11_PROBE_RENDER_H
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
int x11_render(PFN_vkGetInstanceProcAddr, xcb_connection_t *, xcb_window_t, xcb_visualid_t, Display *);

int x11_resize(PFN_vkGetInstanceProcAddr, VkInstance, VkPhysicalDevice, VkDevice, VkQueue,
    xcb_connection_t *, xcb_window_t, VkSwapchainCreateInfoKHR *, VkSwapchainKHR *,
    VkCommandBuffer, VkFence, const VkSemaphore *, unsigned, unsigned, unsigned);

#endif
