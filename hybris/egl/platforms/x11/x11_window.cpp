/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-config.h>
#include <hardware/gralloc.h>
#include "x11_window.h"
#include "tawc_dri_protocol.h"
#include "logging.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=2 || ANDROID_VERSION_MAJOR>=5
extern "C" {
#include <sync/sync.h>
}
#endif

extern "C" {
#include <xcb/xcbext.h>
}

/* Shared with xcb_send_request_with_fds and
 * xcb_register_for_special_xge; libxcb resolves the major opcode by
 * name at runtime and caches it per xcb_extension_t. One object for
 * the whole plugin (eglplatform_x11.cpp uses it too) so libxcb only
 * caches the extension data once. */
xcb_extension_t tawc_dri_ext = { TAWC_DRI_NAME, 0 };

#define ARDESK_PRESENT_MAGIC_HANDLE 0x33424841u /* 'AHB3' */

static int ardesk_present_connect(void)
{
    const char *path = getenv("PRESENT_SOCKET");
    char buf[512];
    int i;

    if (!path || !path[0]) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (!xdg || !xdg[0])
            return -1;
        snprintf(buf, sizeof(buf), "%s/present.sock", xdg);
        path = buf;
    }
    for (i = 0; i < 20; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        if (fd < 0)
            return -1;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        close(fd);
        usleep(100000);
    }
    return -1;
}

static int ardesk_present_handle(xcb_window_t xwin, const native_handle_t *nh,
                                 unsigned int w, unsigned int h,
                                 unsigned int stride, unsigned int format,
                                 uint64_t usage)
{
    static int sock = -1;
    struct {
        uint32_t magic, kind, id, width, height, stride, fmt;
        uint32_t usage_lo, usage_hi;
        int32_t num_fds, num_ints;
    } hdr;
    uint32_t ack = 1;
    int num_fds, num_ints;
    const uint8_t *p;
    size_t n;

    if (!nh)
        return -1;
    if (sock < 0)
        sock = ardesk_present_connect();
    if (sock < 0)
        return -1;
    num_fds = nh->numFds;
    num_ints = nh->numInts;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ARDESK_PRESENT_MAGIC_HANDLE;
    hdr.kind = 1; /* PRESENT_AHB_X11 */
    hdr.id = (uint32_t)xwin;
    hdr.width = w;
    hdr.height = h;
    hdr.stride = stride;
    hdr.fmt = format;
    hdr.usage_lo = (uint32_t)(usage & 0xffffffffULL);
    hdr.usage_hi = (uint32_t)(usage >> 32);
    hdr.num_fds = num_fds;
    hdr.num_ints = num_ints;
    p = (const uint8_t *)&hdr;
    n = sizeof(hdr);
    while (n) {
        ssize_t wr = write(sock, p, n);
        if (wr < 0) {
            if (errno == EINTR)
                continue;
            close(sock);
            sock = -1;
            return -1;
        }
        p += (size_t)wr;
        n -= (size_t)wr;
    }
    if (num_ints > 0) {
        p = (const uint8_t *)(nh->data + num_fds);
        n = (size_t)num_ints * sizeof(int32_t);
        while (n) {
            ssize_t wr = write(sock, p, n);
            if (wr < 0) {
                if (errno == EINTR)
                    continue;
                close(sock);
                sock = -1;
                return -1;
            }
            p += (size_t)wr;
            n -= (size_t)wr;
        }
    }
    if (num_fds > 0) {
        struct iovec iov;
        union {
            struct cmsghdr hdr;
            char space[CMSG_SPACE(32 * sizeof(int))];
        } cmsgbuf;
        struct msghdr msg;
        struct cmsghdr *cmsg;
        int fds[32];
        uint8_t dummy = 0;

        if (num_fds > 32)
            return -1;
        memcpy(fds, nh->data, (size_t)num_fds * sizeof(int));
        iov.iov_base = &dummy;
        iov.iov_len = 1;
        memset(&msg, 0, sizeof(msg));
        memset(&cmsgbuf, 0, sizeof(cmsgbuf));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = &cmsgbuf;
        msg.msg_controllen = CMSG_SPACE((size_t)num_fds * sizeof(int));
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN((size_t)num_fds * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds, (size_t)num_fds * sizeof(int));
        if (sendmsg(sock, &msg, 0) < 0) {
            close(sock);
            sock = -1;
            return -1;
        }
    }
    {
        uint8_t *q = (uint8_t *)&ack;
        n = sizeof(ack);
        while (n) {
            ssize_t rd = read(sock, q, n);
            if (rd <= 0) {
                if (rd < 0 && errno == EINTR)
                    continue;
                close(sock);
                sock = -1;
                return -1;
            }
            q += (size_t)rd;
            n -= (size_t)rd;
        }
    }
    return ack == 0 ? 0 : -1;
}

X11NativeWindowBuffer::X11NativeWindowBuffer(unsigned int w,
                                             unsigned int h,
                                             unsigned int fmt,
                                             uint64_t usg)
    : busy(0)
    , youngest(0)
    , serial(0)
{
    ANativeWindowBuffer::width  = w;
    ANativeWindowBuffer::height = h;
    ANativeWindowBuffer::format = fmt;
    ANativeWindowBuffer::usage  = usg;
    int rc = hybris_gralloc_allocate(w ? w : 1,
                                     h ? h : 1,
                                     fmt, (uint32_t)usg,
                                     &this->handle, (uint32_t *)&this->stride);
    assert(rc == 0);
    this->common.incRef(&this->common);
}

X11NativeWindowBuffer::~X11NativeWindowBuffer()
{
    if (this->handle)
        hybris_gralloc_release(this->handle, 1);
}

X11NativeWindow::X11NativeWindow(xcb_connection_t *conn,
                                 xcb_window_t xwin,
                                 uint8_t tawc_dri_opcode,
                                 unsigned int w,
                                 unsigned int h,
                                 bool server_v03,
                                 bool present_sock)
    : m_conn(conn)
    , m_xwin(xwin)
    , m_tawc_dri_opcode(tawc_dri_opcode)
    , m_width(w ? w : 1)
    , m_height(h ? h : 1)
    , m_format(HAL_PIXEL_FORMAT_RGBA_8888)
    , m_usage(GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE)
    , m_swap_interval(1)
    , m_server_v03(server_v03)
    , m_present_sock(present_sock)
    , m_events_enabled(false)
    , m_eid(0)
    , m_special_ev(NULL)
    , m_next_serial(0)
{
    const_cast<int &>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int &>(ANativeWindow::maxSwapInterval) = 1;
    pthread_mutex_init(&m_mutex, NULL);
    if (m_server_v03 && !m_present_sock)
        setupEventChannel();
    setBufferCount(3);
}

/* Register a libxcb special event queue keyed by a fresh eid and
 * select TAWC-DRI events on the window. Special-event filtering
 * happens inside libxcb before Xlib sees anything, so this works on
 * the app-owned (Xlib) connection without touching its event queue —
 * the same mechanism Present events use for Mesa's DRI3 loader.
 *
 * The server immediately answers the SelectInput with one
 * ConfigureNotify carrying the window's current size, closing the
 * race where a WM resize lands between our GetGeometry and this
 * registration. */
void X11NativeWindow::sendSelectInput(uint32_t mask)
{
    tawc_dri_select_input_req req = {};
    req.length     = sizeof(req) / 4;
    req.eid        = m_eid;
    req.window     = (uint32_t)m_xwin;
    req.event_mask = mask;
    struct iovec iov[3];
    iov[2].iov_base = &req;
    iov[2].iov_len  = sizeof(req);
    xcb_protocol_request_t si_req = {};
    si_req.count  = 1;
    si_req.ext    = &tawc_dri_ext;
    si_req.opcode = X_TAWCDRI_SelectInput;
    si_req.isvoid = 1;
    (void)xcb_send_request(m_conn, 0, &iov[2], &si_req);
    xcb_flush(m_conn);
}

void X11NativeWindow::setupEventChannel()
{
    m_eid = xcb_generate_id(m_conn);
    m_special_ev =
        xcb_register_for_special_xge(m_conn, &tawc_dri_ext, m_eid, NULL);
    if (!m_special_ev) {
        HYBRIS_ERROR("x11-platform: xcb_register_for_special_xge failed; "
                     "window won't follow resizes");
        return;
    }

    sendSelectInput(TAWC_DRI_EVENT_MASK_CONFIGURE_NOTIFY |
                    TAWC_DRI_EVENT_MASK_BUFFER_RELEASE);
    m_events_enabled = true;
}

X11NativeWindow::~X11NativeWindow()
{
    if (m_special_ev) {
        /* Deselect so the server stops writing events for this eid. */
        if (m_events_enabled && !xcb_connection_has_error(m_conn))
            sendSelectInput(0);
        xcb_unregister_for_special_event(m_conn, m_special_ev);
    }
    destroyBuffers();
    pthread_mutex_destroy(&m_mutex);
}

/* Handle one TAWC-DRI XGE special event. Called with m_mutex held. */
void X11NativeWindow::handleSpecialEvent(void *generic_event)
{
    xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)generic_event;

    switch (ge->event_type) {
    case TAWC_DRI_EVENT_CONFIGURE_NOTIFY: {
        tawc_dri_configure_notify_event *cn =
            (tawc_dri_configure_notify_event *)generic_event;
        if (cn->width && cn->height) {
            m_width  = (int)cn->width;
            m_height = (int)cn->height;
            /* dequeueBuffer's dimension-mismatch check reallocates
             * lazily; presentBuffer sends per-buffer dims, so the
             * whole downstream pipe follows automatically. */
        }
        break;
    }
    case TAWC_DRI_EVENT_BUFFER_RELEASE: {
        tawc_dri_buffer_release_event *br =
            (tawc_dri_buffer_release_event *)generic_event;
        /* serial 0 is never assigned (a presented buffer always gets a
         * fresh non-zero serial and it's cleared on dequeue/force-free),
         * so a stale release after a force-free can't match a buffer
         * the GPU has since re-acquired. */
        if (br->serial == 0)
            break;
        for (auto *b : m_bufList) {
            if (b->busy && b->serial == br->serial) {
                b->busy = 0;
                b->serial = 0;
                break;
            }
        }
        break;
    }
    }
}

/* Drain pending special events without blocking. Called with m_mutex
 * held; xcb reads the socket itself under its own iolock. */
void X11NativeWindow::drainSpecialEvents()
{
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_special_event(m_conn, m_special_ev))) {
        handleSpecialEvent(ev);
        free(ev);
    }
}

bool X11NativeWindow::haveFreeBuffer() const
{
    for (auto *b : m_bufList)
        if (!b->busy)
            return true;
    return false;
}

/* Give up waiting for BufferRelease events: free every buffer that was
 * presented and is still awaiting its release (serial != 0). Buffers
 * currently dequeued by the GL driver are also busy but carry serial 0;
 * those must NOT be freed — handing one out twice means two owners
 * writing one AHB. Called with m_mutex held. */
void X11NativeWindow::forceFreePresented(const char *why)
{
    int freed = 0;
    for (auto *b : m_bufList) {
        if (b->busy && b->serial != 0) {
            b->busy = 0;
            b->serial = 0;
            ++freed;
        }
    }
    HYBRIS_WARN("x11-platform: %s; force-freed %d presented buffer(s)",
                why, freed);
}

void X11NativeWindow::lock()   { pthread_mutex_lock(&m_mutex); }
void X11NativeWindow::unlock() { pthread_mutex_unlock(&m_mutex); }

unsigned int X11NativeWindow::width()         const { return m_width;  }
unsigned int X11NativeWindow::height()        const { return m_height; }
unsigned int X11NativeWindow::format()        const { return m_format; }
unsigned int X11NativeWindow::defaultWidth()  const { return m_width;  }
unsigned int X11NativeWindow::defaultHeight() const { return m_height; }
unsigned int X11NativeWindow::queueLength()   const { return 1; }
unsigned int X11NativeWindow::transformHint() const { return 0; }
unsigned int X11NativeWindow::getUsage()      const { return m_usage; }

unsigned int X11NativeWindow::type() const
{
#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=3 || ANDROID_VERSION_MAJOR>=5
    return NATIVE_WINDOW_SURFACE;
#else
    return NATIVE_WINDOW_SURFACE_TEXTURE_CLIENT;
#endif
}

int X11NativeWindow::setSwapInterval(int interval)
{
    if (interval < 0) interval = 0;
    if (interval > 1) interval = 1;
    lock();
    m_swap_interval = interval;
    unlock();
    return 0;
}

int X11NativeWindow::setBuffersFormat(int fmt)
{
    lock();
    if (fmt != m_format) {
        m_format = fmt;
        /* re-allocated lazily on next dequeue */
    }
    unlock();
    return NO_ERROR;
}

int X11NativeWindow::setBuffersDimensions(int w, int h)
{
    lock();
    if (m_width != w || m_height != h) {
        m_width = w;
        m_height = h;
    }
    unlock();
    return NO_ERROR;
}

void X11NativeWindow::resize(unsigned int w, unsigned int h)
{
    lock();
    m_width = w;
    m_height = h;
    unlock();
}

int X11NativeWindow::setUsage(uint64_t usg)
{
    usg |= GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    lock();
    if (usg != m_usage) {
        m_usage = usg;
    }
    unlock();
    return NO_ERROR;
}

X11NativeWindowBuffer *X11NativeWindow::addBuffer()
{
    X11NativeWindowBuffer *wnb =
        new X11NativeWindowBuffer(m_width, m_height, m_format, m_usage);
    m_bufList.push_back(wnb);
    return wnb;
}

void X11NativeWindow::destroyBuffer(X11NativeWindowBuffer *wnb)
{
    if (!wnb) return;
    wnb->common.decRef(&wnb->common);
}

void X11NativeWindow::destroyBuffers()
{
    for (auto *wnb : m_bufList)
        destroyBuffer(wnb);
    m_bufList.clear();
    m_queue.clear();
}

int X11NativeWindow::setBufferCount(int cnt)
{
    if ((int)m_bufList.size() == cnt)
        return NO_ERROR;
    lock();
    if ((int)m_bufList.size() > cnt) {
        auto it = m_bufList.begin();
        for (int i = 0; i <= (int)m_bufList.size() - cnt; ++i) {
            destroyBuffer(*it);
            ++it;
            m_bufList.pop_front();
        }
    } else {
        for (int i = (int)m_bufList.size(); i < cnt; ++i)
            (void)addBuffer();
    }
    unlock();
    return NO_ERROR;
}

int X11NativeWindow::dequeueBuffer(BaseNativeWindowBuffer **buffer, int *fenceFd)
{
    lock();

    if (m_events_enabled) {
        drainSpecialEvents();

        /* Real backpressure: every buffer is out with the server, so
         * wait for a BufferRelease. Sanity timeout so a wedged/killed
         * server degrades to the old free-immediately behaviour
         * instead of deadlocking the client. */
        const long TIMEOUT_MS = 500;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!haveFreeBuffer()) {
            if (xcb_connection_has_error(m_conn)) {
                forceFreePresented("X connection broken while waiting "
                                   "for BufferRelease");
                break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000 +
                              (now.tv_nsec - t0.tv_nsec) / 1000000;
            if (elapsed_ms >= TIMEOUT_MS) {
                forceFreePresented("no BufferRelease within 500ms");
                break;
            }
            /* Poll in short slices: another app thread (a toolkit's X
             * event thread) may read the socket and queue our release
             * into libxcb's special-event queue while the fd shows no
             * data — a full-timeout poll would sleep through it. Each
             * slice re-drains the special queue. */
            long slice_ms = TIMEOUT_MS - elapsed_ms;
            if (slice_ms > 50)
                slice_ms = 50;
            struct pollfd pfd;
            pfd.fd = xcb_get_file_descriptor(m_conn);
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, (int)slice_ms) < 0 && errno != EINTR) {
                forceFreePresented("poll on the X fd failed");
                break;
            }
            drainSpecialEvents();
        }
    }

    /* Pick a free, non-youngest buffer; fall back to any free one. */
    auto it = m_bufList.begin();
    for (; it != m_bufList.end(); ++it) {
        if ((*it)->busy) continue;
        if ((*it)->youngest) continue;
        break;
    }
    if (it == m_bufList.end()) {
        it = m_bufList.begin();
        for (; it != m_bufList.end() && (*it)->busy; ++it) {}
    }
    if (it == m_bufList.end()) {
        unlock();
        return NO_ERROR;
    }

    X11NativeWindowBuffer *wnb = *it;

    /* Re-allocate if dimensions/format changed since last use. */
    if ((unsigned)wnb->width  != (unsigned)m_width ||
        (unsigned)wnb->height != (unsigned)m_height ||
        (unsigned)wnb->format != (unsigned)m_format ||
        wnb->usage  != m_usage)
    {
        destroyBuffer(wnb);
        m_bufList.erase(it);
        wnb = addBuffer();
    }

    wnb->busy = 1;
    /* Handed to the GL driver: not awaiting a release. Keeps a stale
     * serial from matching a late BufferRelease after a force-free. */
    wnb->serial = 0;
    *buffer = wnb;
    if (fenceFd) *fenceFd = -1;

    unlock();
    return NO_ERROR;
}

int X11NativeWindow::lockBuffer(BaseNativeWindowBuffer *)
{
    return NO_ERROR;
}

int X11NativeWindow::cancelBuffer(BaseNativeWindowBuffer *buffer, int fenceFd)
{
    X11NativeWindowBuffer *wnb = static_cast<X11NativeWindowBuffer *>(buffer);
    if (fenceFd >= 0) close(fenceFd);
    lock();
    wnb->busy = 0;
    wnb->serial = 0;
    for (auto *b : m_bufList) b->youngest = 0;
    wnb->youngest = 1;
    unlock();
    return 0;
}

void X11NativeWindow::prepareSwap(EGLint *, EGLint)
{
    /* TAWC-DRI doesn't carry damage rects today; the X server presents
     * full-buffer attaches. Hook reserved for parity with the wayland
     * platform when the protocol grows damage support. */
}

void X11NativeWindow::finishSwap()
{
    /* All work happens in queueBuffer (see comment in upstream
     * libhybris's wayland-egl: attach+commit inside queueBuffer so
     * drivers that skip eglSwapBuffers still submit). Nothing to do
     * here; the present already went out on the wire. */
}

/* Build a TAWCDRIPresentBuffer request and ship the fds out-of-band
 * via xcb_send_request_with_fds. xcb dups the fds and closes its
 * copies after sending — we keep the AHB-owned originals. */
int X11NativeWindow::presentBuffer(X11NativeWindowBuffer *wnb)
{
    const native_handle_t *nh = wnb->handle;
    if (!nh) {
        HYBRIS_ERROR("x11-platform: buffer has no native_handle");
        return -1;
    }
    if (m_present_sock) {
        return ardesk_present_handle(m_xwin, nh,
                                     (unsigned int)wnb->width,
                                     (unsigned int)wnb->height,
                                     (unsigned int)wnb->stride,
                                     (unsigned int)wnb->format,
                                     wnb->usage);
    }
    int num_fds  = nh->numFds;
    int num_ints = nh->numInts;

    /* v0.2 servers take the request without the trailing serial. */
    size_t hdr_sz  = m_server_v03
        ? sizeof(tawc_dri_present_buffer_req)
        : sizeof(tawc_dri_present_buffer_req) - sizeof(uint32_t);
    size_t ints_sz = (size_t)num_ints * sizeof(int32_t);
    size_t body_sz = hdr_sz + ints_sz;
    /* xcb pads up via the ((body_sz + 3) / 4) length, so we pass
     * the unpadded body and let xcb handle alignment. */
    char body[body_sz > 256 ? 1 : 256];
    char *body_p = body_sz <= sizeof(body) ? body : (char *)malloc(body_sz);
    if (!body_p) return -1;

    tawc_dri_present_buffer_req hdr = {};
    hdr.length   = (uint16_t)((body_sz + 3) / 4);
    hdr.window   = (uint32_t)m_xwin;
    hdr.num_fds  = (uint16_t)num_fds;
    hdr.num_ints = (uint16_t)num_ints;
    hdr.width    = (uint32_t)wnb->width;
    hdr.height   = (uint32_t)wnb->height;
    hdr.stride   = (uint32_t)wnb->stride;
    hdr.format   = (uint32_t)wnb->format;
    hdr.usage_lo = (uint32_t)(wnb->usage & 0xffffffffULL);
    hdr.usage_hi = (uint32_t)((uint64_t)wnb->usage >> 32);
    hdr.serial   = wnb->serial;
    memcpy(body_p, &hdr, hdr_sz);
    if (ints_sz > 0)
        memcpy(body_p + hdr_sz, nh->data + num_fds, ints_sz);

    struct iovec iov[3];
    iov[2].iov_base = body_p;
    iov[2].iov_len  = body_sz;

    int *xfds = NULL;
    if (num_fds > 0) {
        xfds = (int *)malloc(num_fds * sizeof(int));
        if (!xfds) {
            if (body_p != body) free(body_p);
            return -1;
        }
        for (int i = 0; i < num_fds; i++) {
            xfds[i] = dup(nh->data[i]);
            if (xfds[i] < 0) {
                while (--i >= 0) close(xfds[i]);
                free(xfds);
                if (body_p != body) free(body_p);
                return -1;
            }
        }
    }

    xcb_protocol_request_t pb_req = {};
    pb_req.count  = 1;
    pb_req.ext    = &tawc_dri_ext;
    pb_req.opcode = X_TAWCDRI_PresentBuffer;
    pb_req.isvoid = 1;

    (void)xcb_send_request_with_fds(m_conn, 0, &iov[2], &pb_req,
                                    num_fds, xfds);
    free(xfds);
    xcb_flush(m_conn);
    if (body_p != body) free(body_p);
    return 0;
}

int X11NativeWindow::queueBuffer(BaseNativeWindowBuffer *buffer, int fenceFd)
{
    X11NativeWindowBuffer *wnb = static_cast<X11NativeWindowBuffer *>(buffer);

#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=2 || ANDROID_VERSION_MAJOR>=5
    if (fenceFd >= 0) {
        sync_wait(fenceFd, -1);
        close(fenceFd);
    }
#endif

    lock();
    if (m_events_enabled) {
        if (++m_next_serial == 0)
            ++m_next_serial; /* 0 means "not awaiting release" */
        wnb->serial = m_next_serial;
    }
    int rc = presentBuffer(wnb);

    if (m_events_enabled && rc == 0) {
        /* v0.3: the buffer stays busy until its TAWCDRIBufferRelease
         * arrives (the X server forwards the compositor's
         * wl_buffer.release). dequeueBuffer blocks when every buffer
         * is out — real backpressure instead of a freshness
         * heuristic. */
    }
    else {
        /* Pre-0.3 fallback (no release feedback exists; the 3-buffer
         * round-robin gives the compositor enough breathing room
         * before a slot is reused) — and the failed-present path: if
         * nothing went out on the wire, no release will ever arrive,
         * so leaving the buffer busy would strand the slot forever. */
        wnb->busy = 0;
        wnb->serial = 0;
    }
    for (auto *b : m_bufList) b->youngest = 0;
    wnb->youngest = 1;
    unlock();
    return rc;
}
