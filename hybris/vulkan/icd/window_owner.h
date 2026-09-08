/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_VULKAN_WAYLAND_OWNER_H
#define HYBRIS_VULKAN_WAYLAND_OWNER_H

#include <stdint.h>

struct wl_display;
struct wl_surface;
struct ANativeWindow;
struct ANativeWindowBuffer;
struct hybris_vk_wayland_window;

#ifdef __cplusplus
extern "C" {
#endif
/* Internal native-window factory, independent of either Vulkan loader.
 * The caller retains display/surface until destroy. Creation uses a private
 * discovery queue and requires android_wlegl. Returns zero or negative errno.
 * Gralloc must already be initialized. Native window access is borrowed;
 * destroy all Vulkan surfaces/swapchains that reference it before destroy. */
int hybris_vk_wayland_window_create(struct wl_display *display,
    struct wl_surface *surface, struct hybris_vk_wayland_window **out);
struct ANativeWindow *hybris_vk_wayland_window_native(struct hybris_vk_wayland_window *window);
void hybris_vk_wayland_window_resize(struct hybris_vk_wayland_window *window,
    unsigned width, unsigned height);
int hybris_vk_wayland_window_dequeue(struct hybris_vk_wayland_window *window,
    int64_t timeout_ns, struct ANativeWindowBuffer **buffer, int *fence_fd);
/* Queue and cancel consume fence_fd on every return path. */
int hybris_vk_wayland_window_queue(struct hybris_vk_wayland_window *window,
    struct ANativeWindowBuffer *buffer, int fence_fd);
int hybris_vk_wayland_window_cancel(struct hybris_vk_wayland_window *window,
    struct ANativeWindowBuffer *buffer, int fence_fd);
void hybris_vk_wayland_window_disconnect(struct hybris_vk_wayland_window *window);
void hybris_vk_wayland_window_destroy(struct hybris_vk_wayland_window *window);
#ifdef __cplusplus
}
#endif
#endif
