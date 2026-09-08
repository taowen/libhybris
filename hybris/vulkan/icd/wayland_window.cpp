/****************************************************************************************
 **
 ** Copyright (C) 2013-2022 Jolla Ltd.
 ** All rights reserved.
 **
 ** This file is part of Wayland enablement for libhybris
 **
 ** You may use this file under the terms of the GNU Lesser General
 ** Public License version 2.1 as published by the Free Software Foundation
 ** and appearing in the file license.lgpl included in the packaging
 ** of this file.
 **
 ** This library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public
 ** License version 2.1 as published by the Free Software Foundation
 ** and appearing in the file license.lgpl included in the packaging
 ** of this file.
 **
 ** This library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ****************************************************************************************/


#include <android-config.h>
#include <hardware/gralloc.h>
#include "wayland_window.h"
#include <algorithm>
#include <wayland-egl-backend.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include <vulkanhybris.h>

#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=2 || ANDROID_VERSION_MAJOR>=5
extern "C" {
#include <sync/sync.h>
}
#endif

static void
wl_buffer_release(void *data, struct wl_buffer *buffer)
{
    WaylandNativeWindow *win = static_cast<WaylandNativeWindow *>(data);
    win->releaseBuffer(buffer);
}

static struct wl_buffer_listener wl_buffer_listener = {
    wl_buffer_release
};

static void
wayland_frame_callback(void *data, struct wl_callback *callback, uint32_t time)
{
    WaylandNativeWindow *surface = static_cast<WaylandNativeWindow *>(data);
    surface->frame();
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
    wayland_frame_callback
};

void WaylandNativeWindow::destroyWlEGLWindow()
{
    wl_egl_window_destroy(m_window);
}

int WaylandNativeWindow::apiDisconnect(int api)
{
    (void)api;
    HYBRIS_TRACE_BEGIN("wayland-platform", "producer_disconnect", "window=%p", this);
    lock();
    wl_display_dispatch_queue_pending(m_display, wl_queue);
    // Android Vulkan reconnects before allocating a replacement swapchain.
    // A displayed buffer may not be released until a NEW buffer is committed.
    // Retire those buffers outside the new producer pool; never wait for them
    // here and never return them to a producer from the new connection.
    unsigned retained = 0;
    for (auto *buffer : m_bufList) {
        if (std::find(fronted.begin(), fronted.end(), buffer) != fronted.end())
            ++retained;
        else
            destroyBuffer(buffer);
    }
    m_bufList.clear();
    m_freeBufs = 0;
    if (frame_callback) {
        wl_callback_destroy(frame_callback);
        frame_callback = NULL;
    }
    HYBRIS_TRACE_COUNTER("wayland-platform", "retired_buffers", "%u", retained);
    HYBRIS_TRACE_END("wayland-platform", "producer_disconnect", "");
    unlock();
    return NO_ERROR;
}

int WaylandNativeWindow::dequeueBuffer(BaseNativeWindowBuffer **buffer, int *fenceFd){
    HYBRIS_TRACE_BEGIN("wayland-platform", "dequeueBuffer", "");

    WaylandNativeWindowBuffer *wnb=NULL;
    TRACE("%p", buffer);

    lock();
    readQueue(false);

    HYBRIS_TRACE_BEGIN("wayland-platform", "dequeueBuffer_wait_for_buffer", "");

    HYBRIS_TRACE_COUNTER("wayland-platform", "m_freeBufs", "%i", m_freeBufs);

    while (m_freeBufs==0) {
        HYBRIS_TRACE_COUNTER("wayland-platform", "m_freeBufs", "%i", m_freeBufs);
        readQueue(true);
    }

    std::list<WaylandNativeWindowBuffer *>::iterator it = m_bufList.begin();
    for (; it != m_bufList.end(); ++it)
    {
         if ((*it)->busy)
             continue;
         if ((*it)->youngest == 1)
             continue;
         break;
    }

    if (it==m_bufList.end()) {
        HYBRIS_TRACE_BEGIN("wayland-platform", "dequeueBuffer_worst_case_scenario", "");
        HYBRIS_TRACE_END("wayland-platform", "dequeueBuffer_worst_case_scenario", "");

        it = m_bufList.begin();
        for (; it != m_bufList.end() && (*it)->busy; ++it)
        {}

    }
    if (it==m_bufList.end()) {
        *buffer = 0;
        *fenceFd = -1;
        unlock();
        HYBRIS_TRACE_BEGIN("wayland-platform", "dequeueBuffer_no_free_buffers", "");
        HYBRIS_TRACE_END("wayland-platform", "dequeueBuffer_no_free_buffers", "");
        TRACE("%p: no free buffers", buffer);
        return NO_ERROR;
    }

    wnb = *it;
    assert(wnb!=NULL);
    HYBRIS_TRACE_END("wayland-platform", "dequeueBuffer_wait_for_buffer", "");

    /* If the buffer doesn't match the window anymore, re-allocate */
    if (wnb->width != m_width || wnb->height != m_height
        || wnb->format != m_format || wnb->usage != m_usage)
    {
        TRACE("wnb:%p,win:%p %i,%i %i,%i x%x,x%x x%x,x%" PRIx64,
            wnb,m_window,
            wnb->width,m_width, wnb->height,m_height,
            wnb->format,m_format, wnb->usage, m_usage);
        destroyBuffer(wnb);
        m_bufList.erase(it);
        wnb = addBuffer();
    }

    wnb->busy = 1;
    *buffer = wnb;
    --m_freeBufs;

    HYBRIS_TRACE_COUNTER("wayland-platform", "m_freeBufs", "%i", m_freeBufs);
    HYBRIS_TRACE_BEGIN("wayland-platform", "dequeueBuffer_gotBuffer", "-%p", wnb);
    HYBRIS_TRACE_END("wayland-platform", "dequeueBuffer_gotBuffer", "-%p", wnb);
    HYBRIS_TRACE_END("wayland-platform", "dequeueBuffer_wait_for_buffer", "");

    *fenceFd = -1;

    unlock();
    return NO_ERROR;
}

int WaylandNativeWindow::dequeueBufferTimeout(BaseNativeWindowBuffer **buffer, int *fenceFd,
    int64_t timeout_ns)
{
    HYBRIS_TRACE_BEGIN("wayland-platform", "dequeueBufferTimeout", "");
    *buffer = nullptr;
    *fenceFd = -1;
    lock();
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    int error = 0;
    bool polled = false;
    for (;;) {
        if (wl_display_dispatch_queue_pending(m_display, wl_queue) < 0) {
            error = -EPIPE;
            break;
        }
        if (m_freeBufs) break;
        // Register the reader before polling; pending dispatch alone never
        // reads release events from the socket and races with other queues.
        if (wl_display_prepare_read_queue(m_display, wl_queue) != 0) {
            if (errno == EAGAIN) continue;
            error = -EPIPE;
            break;
        }
        int timeout_ms = -1;
        if (timeout_ns >= 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsed = (int64_t)(now.tv_sec - started.tv_sec) * 1000000000LL +
                now.tv_nsec - started.tv_nsec;
            if ((timeout_ns > 0 && elapsed >= timeout_ns) || (!timeout_ns && polled)) {
                wl_display_cancel_read(m_display);
                error = timeout_ns == 0 ? -EAGAIN : -ETIMEDOUT;
                break;
            }
            int64_t remain = elapsed >= timeout_ns ? 0 : timeout_ns - elapsed;
            // Saturate without overflowing either the addition or int poll timeout.
            int64_t milliseconds = remain / 1000000LL + (remain % 1000000LL != 0);
            timeout_ms = milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
        }
        short events = POLLIN;
        if (wl_display_flush(m_display) < 0) {
            if (errno == EAGAIN) events |= POLLOUT;
            else {
                wl_display_cancel_read(m_display);
                error = -EPIPE;
                break;
            }
        }
        struct pollfd wait = {wl_display_get_fd(m_display), events, 0};
        int status = poll(&wait, 1, timeout_ms);
        polled = true;
        if (status <= 0 || !(wait.revents & POLLIN)) {
            wl_display_cancel_read(m_display);
            if (status < 0 && errno == EINTR) continue;
            if (status < 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                error = -EPIPE;
                break;
            }
            if (!status) {
                // A saturated INT_MAX interval can expire before the deadline.
                if (timeout_ms == INT_MAX) continue;
                error = timeout_ns == 0 ? -EAGAIN : -ETIMEDOUT;
                break;
            }
            continue; // Writable: flush again before the next read preparation.
        }
        if (wl_display_read_events(m_display) < 0) {
            error = -EPIPE;
            break;
        }
    }
    if (error) {
        unlock();
        HYBRIS_TRACE_END("wayland-platform", "dequeueBufferTimeout", "");
        return error;
    }

    std::list<WaylandNativeWindowBuffer *>::iterator it = m_bufList.begin();
    for (; it != m_bufList.end(); ++it)
    {
         if ((*it)->busy)
             continue;
         if ((*it)->youngest == 1)
             continue;
         break;
    }
    if (it==m_bufList.end()) {
        it = m_bufList.begin();
        for (; it != m_bufList.end() && (*it)->busy; ++it)
        {}
    }
    if (it==m_bufList.end()) {
        *buffer = 0;
        *fenceFd = -1;
        unlock();
        HYBRIS_TRACE_END("wayland-platform", "dequeueBufferTimeout", "");
        return -EAGAIN;
    }
    WaylandNativeWindowBuffer *wnb = *it;
    if (wnb->width != m_width || wnb->height != m_height
        || wnb->format != m_format || wnb->usage != m_usage)
    {
        destroyBuffer(wnb);
        m_bufList.erase(it);
        wnb = addBuffer();
    }
    wnb->busy = 1;
    *buffer = wnb;
    --m_freeBufs;
    *fenceFd = -1;
    unlock();
    HYBRIS_TRACE_END("wayland-platform", "dequeueBufferTimeout", "");
    return 0;
}

int WaylandNativeWindow::presentBuffer(WaylandNativeWindowBuffer *wnb)
{
    int ret = 0;
    if (!m_window) {
        return -EPIPE;
    }

    ret = wl_display_dispatch_queue_pending(m_display, wl_queue);
    if (this->frame_callback) {
        do {
            ret = wl_display_dispatch_queue(m_display, wl_queue);
        } while (this->frame_callback && ret != -1);
    }
    if (ret < 0) {
        HYBRIS_TRACE_END("wayland-platform", "queueBuffer_wait_for_frame_callback", "");
        return -EPIPE;
    }

    if (m_swap_interval > 0) {
        this->frame_callback = wl_surface_frame(wl_surface_wrapper);
        wl_callback_add_listener(this->frame_callback, &frame_listener, this);
    }

    if (wnb) {
        assert(wnb->busy == 1);

        if (!wnb->wlbuffer) {
            wnb->init(m_android_wlegl, m_display, wl_queue);
            TRACE("%p add listener with %p inside", wnb, wnb->wlbuffer);
            wl_buffer_add_listener(wnb->wlbuffer, &wl_buffer_listener, this);
        }

        wl_surface_attach(wl_surface_wrapper, wnb->wlbuffer, 0, 0);

        m_window->attached_width = wnb->width;
        m_window->attached_height = wnb->height;

        fronted.push_back(wnb);
    }

    // If the compositor doesn't support damage_buffer, we deliberately
    // ignore the damage region and post maximum damage, due to
    // https://bugs.freedesktop.org/78190
    if (wl_proxy_get_version((struct wl_proxy *) wl_surface_wrapper) >=
        WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        if (m_damage_n_rects > 0 && m_window->attached_height > 0) {
            for (size_t i = 0; i < m_damage_n_rects; i++) {
                const android_native_rect_t *rect = &m_damage_rects[i];
                wl_surface_damage_buffer(wl_surface_wrapper,
                                         rect->left, m_window->attached_height - rect->top - rect->bottom,
                                         rect->right, rect->bottom);
            }
        } else if (wnb) {
            wl_surface_damage_buffer(wl_surface_wrapper, 0, 0, INT32_MAX, INT32_MAX);
        }
    } else if (wnb) {
        wl_surface_damage(wl_surface_wrapper, 0, 0, INT32_MAX, INT32_MAX);
    }

    wl_surface_commit(wl_surface_wrapper);

    // If we're not waiting for a frame callback then we'll at least throttle
    // to a sync callback so that we always give a chance for the compositor to
    // handle the commit and send a release event before checking for a free buffer.
    if (this->frame_callback == NULL) {
        this->frame_callback = wl_display_sync(wl_dpy_wrapper);
        wl_callback_add_listener(this->frame_callback, &frame_listener, this);
    }

    wl_display_flush(m_display);

    // TODO damage areas
    m_damage_rects = NULL;
    m_damage_n_rects = 0;
    return 0;
}

static int debugenvchecked = 0;

int WaylandNativeWindow::queueBuffer(BaseNativeWindowBuffer* buffer, int fenceFd)
{
    WaylandNativeWindowBuffer *wnb = (WaylandNativeWindowBuffer*) buffer;

    HYBRIS_TRACE_BEGIN("wayland-platform", "queueBuffer", "-%p", wnb);
    lock();

    if (debugenvchecked == 0)
    {
        if (getenv("HYBRIS_WAYLAND_DUMP_BUFFERS") != NULL)
            debugenvchecked = 2;
        else
            debugenvchecked = 1;
    }
    if (debugenvchecked == 2)
    {
        HYBRIS_TRACE_BEGIN("wayland-platform", "queueBuffer_dumping_buffer", "-%p", wnb);
        hybris_dump_buffer_to_file(wnb->getNativeBuffer());
        HYBRIS_TRACE_END("wayland-platform", "queueBuffer_dumping_buffer", "-%p", wnb);

    }

#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=2 || ANDROID_VERSION_MAJOR>=5
    HYBRIS_TRACE_BEGIN("wayland-platform", "queueBuffer_waiting_for_fence", "-%p", wnb);
    if (fenceFd >= 0)
    {
        int status = sync_wait(fenceFd, -1);
        int error = errno;
        close(fenceFd);
        if (status < 0) {
            unlock();
            return -error;
        }
    }
    HYBRIS_TRACE_END("wayland-platform", "queueBuffer_waiting_for_fence", "-%p", wnb);
#endif

    // Fence wait must precede attach+commit. The Wayland compositor sees the
    // buffer the instant we commit; it has no linux_drm_syncobj or equivalent
    // to wait on per-commit fences. If we committed while the GPU was still
    // writing, the compositor would composite whatever partial/empty content
    // was in the buffer at that moment.
    bool retired = std::find(m_bufList.begin(), m_bufList.end(), wnb) == m_bufList.end();
    if (retired) wnb->common.incRef(&wnb->common);
    int result = presentBuffer(wnb);
    if (result && retired) wnb->common.decRef(&wnb->common);

    HYBRIS_TRACE_COUNTER("wayland-platform", "fronted.size", "%lu", fronted.size());
    HYBRIS_TRACE_END("wayland-platform", "queueBuffer", "-%p", wnb);
    unlock();

    return result;
}

// vim: noai:ts=4:sw=4:ss=4:expandtab
