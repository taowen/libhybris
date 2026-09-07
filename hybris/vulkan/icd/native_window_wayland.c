/* SPDX-License-Identifier: Apache-2.0 */
#include "native_window.h"
#include "../platforms/wayland/window_owner.h"
#include <system/window.h>
#include <errno.h>
#include <stdlib.h>
struct wayland_owner { struct hybris_icd_window base; struct hybris_vk_wayland_window *window; };
static struct hybris_vk_wayland_window *get(struct hybris_icd_window *owner) {
    return ((struct wayland_owner *)owner)->window;
}
static int configure(struct hybris_icd_window *owner, unsigned width, unsigned height,
    int format, unsigned usage, unsigned count) {
    hybris_vk_wayland_window_resize(get(owner), width, height);
    ANativeWindow *window = hybris_vk_wayland_window_native(get(owner));
    if (window->perform(window, NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS, (int)width, (int)height) ||
        window->perform(window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, format) ||
        window->perform(window, NATIVE_WINDOW_SET_USAGE, (int)usage) ||
        window->perform(window, NATIVE_WINDOW_SET_BUFFER_COUNT, (int)count)) return -EINVAL;
    return 0;
}
static int dequeue(struct hybris_icd_window *o, int64_t t, struct ANativeWindowBuffer **b, int *fd) {
    return hybris_vk_wayland_window_dequeue(get(o), t, b, fd);
}
static int queue(struct hybris_icd_window *o, struct ANativeWindowBuffer *b, int fd) {
    return hybris_vk_wayland_window_queue(get(o), b, fd);
}
static int cancel(struct hybris_icd_window *o, struct ANativeWindowBuffer *b, int fd) {
    return hybris_vk_wayland_window_cancel(get(o), b, fd);
}
static void disconnect(struct hybris_icd_window *o) { hybris_vk_wayland_window_disconnect(get(o)); }
static void destroy(struct hybris_icd_window *o) { hybris_vk_wayland_window_destroy(get(o)); free(o); }
static int extent(struct hybris_icd_window *o, uint32_t *w, uint32_t *h) { (void)o; *w = *h = 0; return 0; }
static const struct hybris_icd_window_ops ops = {configure, dequeue, queue, cancel, disconnect, destroy, extent};
int hybris_icd_window_wayland(struct wl_display *display, struct wl_surface *surface,
    struct hybris_icd_window **out) {
    struct wayland_owner *owner = calloc(1, sizeof(*owner));
    if (!owner) return -ENOMEM;
    int error = hybris_vk_wayland_window_create(display, surface, &owner->window);
    if (error) { free(owner); return error; }
    owner->base.ops = &ops; *out = &owner->base; return 0;
}
