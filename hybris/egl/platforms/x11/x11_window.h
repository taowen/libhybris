/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * libhybris EGL-on-X11 native window. Modeled on
 * hybris/platforms/wayland/wayland_window_common.h +
 * hybris/egl/platforms/wayland/wayland_window.h, but the buffer
 * shipping path is TAWCDRIPresentBuffer over the existing X11
 * connection instead of android_wlegl on a Wayland connection.
 *
 * Buffers are AHardwareBuffers allocated client-side via
 * hybris_gralloc_allocate (the AHB gralloc backend, see
 * libhybris/TAWC_FORK.md "AHB gralloc backend"). On queueBuffer the
 * AHB's native_handle (numFds + numInts + inline ints, with the fds
 * sent out-of-band) is sent to the X server which rebuilds it via
 * AHardwareBuffer_createFromHandle and ships it through android_wlegl
 * to the compositor.
 */

#ifndef LIBHYBRIS_X11_WINDOW_H
#define LIBHYBRIS_X11_WINDOW_H

#include "eglnativewindowbase.h"

#include <hybris/gralloc/gralloc.h>

#include <pthread.h>

#include <list>
#include <deque>

extern "C" {
#include <xcb/xcb.h>
}

/* One xcb_extension_t for the whole plugin (defined in x11_window.cpp);
 * libxcb caches the QueryExtension data per object. */
extern xcb_extension_t tawc_dri_ext;

class X11NativeWindowBuffer : public BaseNativeWindowBuffer
{
public:
    X11NativeWindowBuffer(unsigned int width,
                          unsigned int height,
                          unsigned int format,
                          uint64_t usage);
    ~X11NativeWindowBuffer();

    int busy;
    int youngest;
    /* Non-zero while presented and awaiting TAWCDRIBufferRelease (set
     * in queueBuffer, cleared on release/dequeue/cancel/force-free).
     * Zero on driver-held or free buffers, so a stale release can
     * never free a buffer the GPU has since re-acquired. */
    uint32_t serial;
};

class X11NativeWindow : public EGLBaseNativeWindow
{
public:
    X11NativeWindow(xcb_connection_t *conn,
                    xcb_window_t xwin,
                    uint8_t tawc_dri_opcode,
                    unsigned int width,
                    unsigned int height,
                    bool server_v03,
                    bool present_sock);
    ~X11NativeWindow();

    void prepareSwap(EGLint *damage_rects, EGLint damage_n_rects);
    void finishSwap();
    void resize(unsigned int width, unsigned int height);

    virtual int setSwapInterval(int interval);

protected:
    virtual int dequeueBuffer(BaseNativeWindowBuffer **buffer, int *fenceFd);
    virtual int lockBuffer(BaseNativeWindowBuffer *buffer);
    virtual int queueBuffer(BaseNativeWindowBuffer *buffer, int fenceFd);
    virtual int cancelBuffer(BaseNativeWindowBuffer *buffer, int fenceFd);
    virtual unsigned int type() const;
    virtual unsigned int width() const;
    virtual unsigned int height() const;
    virtual unsigned int format() const;
    virtual unsigned int defaultWidth() const;
    virtual unsigned int defaultHeight() const;
    virtual unsigned int queueLength() const;
    virtual unsigned int transformHint() const;
    virtual unsigned int getUsage() const;
    virtual int setUsage(uint64_t usage);
    virtual int setBuffersFormat(int format);
    virtual int setBuffersDimensions(int width, int height);
    virtual int setBufferCount(int cnt);

private:
    void lock();
    void unlock();
    X11NativeWindowBuffer *addBuffer();
    void destroyBuffer(X11NativeWindowBuffer *wnb);
    void destroyBuffers();
    int presentBuffer(X11NativeWindowBuffer *wnb);
    void setupEventChannel();
    void sendSelectInput(uint32_t mask);
    void handleSpecialEvent(void *generic_event);
    void drainSpecialEvents();
    bool haveFreeBuffer() const;
    void forceFreePresented(const char *why);

    xcb_connection_t *m_conn;
    xcb_window_t m_xwin;
    uint8_t m_tawc_dri_opcode;

    std::list<X11NativeWindowBuffer *> m_bufList;
    std::deque<X11NativeWindowBuffer *> m_queue;

    int m_width;
    int m_height;
    int m_format;
    uint64_t m_usage;
    int m_swap_interval;

    /* TAWC-DRI v0.3 event channel (see tawc_dri_protocol.h). m_server_v03
     * gates the PresentBuffer wire shape (serial field); m_events_enabled
     * additionally requires the special-event registration to have
     * succeeded and gates the event-driven buffer lifecycle. */
    bool m_server_v03;
    bool m_present_sock;
    bool m_events_enabled;
    uint32_t m_eid;
    xcb_special_event_t *m_special_ev;
    uint32_t m_next_serial;

    pthread_mutex_t m_mutex;
};

#endif
