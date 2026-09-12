/* SPDX-License-Identifier: Apache-2.0 */
#include "native_window.h"
#include "../../platforms/common/nativewindowbase.h"
#include <arlinux/tawc-dri.h>
#include <hybris/gralloc/gralloc.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <errno.h>
#include <limits.h>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static xcb_extension_t extension = {TAWC_DRI_NAME, 0};
static int64_t now_ns() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int consume_fence(int fd) {
    if (fd < 0) return 0;
    pollfd p = {fd, POLLIN, 0}; int result;
    do { result = poll(&p, 1, -1); } while (result < 0 && errno == EINTR);
    close(fd);
    return result > 0 && (p.revents & POLLIN) ? 0 : -EIO;
}
struct Buffer : BaseNativeWindowBuffer {
    bool held = false;
    Buffer() { handle = nullptr; }
    ~Buffer() { if (handle) hybris_gralloc_release(handle, 1); }
};
struct Pending { Buffer *buffer; uint32_t serial; Pending *next; };
struct Owner {
    hybris_icd_window base;
    xcb_connection_t *connection;
    xcb_window_t window;
    uint32_t eid, serial, width, height;
    xcb_special_event_t *events;
    Buffer *pool[8];
    unsigned count, pool_width, pool_height;
    bool trace;
    unsigned trace_count;
    Pending *pending;
    pthread_mutex_t mutex;
};
static Owner *get(hybris_icd_window *o) { return reinterpret_cast<Owner *>(o); }
static void trace(Owner *o, const char *event, const Pending *p) {
    if (o->trace && o->trace_count++ < 64)
        fprintf(stderr, "X11_WSI event=%s window=%u serial=%u buffer=%p\n", event, o->window, p->serial, (void *)p->buffer);
}
static int check(xcb_connection_t *connection, unsigned sequence) {
    if (!sequence || xcb_connection_has_error(connection)) return -EPIPE;
    xcb_generic_error_t *error = xcb_request_check(connection, {sequence});
    if (error) { free(error); return -EPIPE; }
    return xcb_connection_has_error(connection) ? -EPIPE : 0;
}
static int select(Owner *o, uint32_t mask) {
    tawc_dri_select_input_req body = {};
    body.eid = o->eid; body.window = o->window; body.event_mask = mask;
    iovec parts[3] = {}; parts[2] = {&body, sizeof(body)};
    xcb_protocol_request_t request = {1, &extension, X_TAWCDRI_SelectInput, 1};
    return check(o->connection, xcb_send_request(o->connection, XCB_REQUEST_CHECKED, parts + 2, &request));
}
static void drain(Owner *o) {
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_special_event(o->connection, o->events))) {
        auto *ge = reinterpret_cast<xcb_ge_generic_event_t *>(event);
        if (ge->event_type == TAWC_DRI_EVENT_CONFIGURE_NOTIFY) {
            auto *configure = reinterpret_cast<tawc_dri_configure_notify_event *>(event);
            o->width = configure->width; o->height = configure->height;
        } else if (ge->event_type == TAWC_DRI_EVENT_BUFFER_RELEASE) {
            uint32_t serial = reinterpret_cast<tawc_dri_buffer_release_event *>(event)->serial;
            Pending **link = &o->pending;
            while (*link && (*link)->serial != serial) link = &(*link)->next;
            if (*link) {
                Pending *p = *link; *link = p->next;
                trace(o, "release", p);
                p->buffer->held = false;
                p->buffer->common.decRef(&p->buffer->common);
                free(p);
            }
        }
        free(event);
    }
}
static void drop_pool(Owner *o) {
    for (unsigned i = 0; i < o->count; ++i) o->pool[i]->common.decRef(&o->pool[i]->common);
    o->count = 0;
}
static void disconnect(hybris_icd_window *base) {
    Owner *o = get(base); pthread_mutex_lock(&o->mutex);
    drain(o); drop_pool(o); pthread_mutex_unlock(&o->mutex);
}
static int configure(hybris_icd_window *base, unsigned width, unsigned height,
    int format, unsigned usage, unsigned count) {
    if (!width || !height || width > INT_MAX || height > INT_MAX || count < 2 || count > 8) return -EINVAL;
    Owner *o = get(base); pthread_mutex_lock(&o->mutex);
    if (o->count) { pthread_mutex_unlock(&o->mutex); return -EBUSY; }
    o->pool_width = width; o->pool_height = height;
    int error = 0;
    for (unsigned i = 0; i < count; ++i) {
        Buffer *b = new (std::nothrow) Buffer;
        if (!b) { error = -ENOMEM; break; }
        b->width = width; b->height = height; b->format = format; b->usage = usage;
        uint32_t stride = 0;
        if (hybris_gralloc_allocate(width, height, format, usage, &b->handle, &stride)) {
            delete b; error = -ENOMEM; break;
        }
        b->stride = stride;
        b->common.incRef(&b->common);
        o->pool[o->count++] = b;
    }
    if (error) drop_pool(o);
    pthread_mutex_unlock(&o->mutex); return error;
}
static int dequeue(hybris_icd_window *base, int64_t timeout, ANativeWindowBuffer **buffer, int *fd) {
    Owner *o = get(base); int64_t start = now_ns();
    *buffer = nullptr; *fd = -1;
    pthread_mutex_lock(&o->mutex);
    int error;
    for (;;) {
        drain(o);
        if (xcb_connection_has_error(o->connection)) { error = -EPIPE; break; }
        /* Configure events change the surface size, not the allocation or
         * release status of this pool. WSI reports SUBOPTIMAL to the caller. */
        for (unsigned i = 0; i < o->count; ++i) {
            if (!o->pool[i]->held) {
                o->pool[i]->held = true; *buffer = o->pool[i];
                pthread_mutex_unlock(&o->mutex); return 0;
            }
        }
        if (!timeout) { error = -EAGAIN; break; }
        int64_t remaining = timeout < 0 ? INT64_MAX : timeout - (now_ns() - start);
        if (remaining <= 0) { error = -ETIMEDOUT; break; }
        /* An application thread can read X events into this private queue
         * while we poll. Periodically drain it without consuming app events. */
        int milliseconds = remaining >= 50000000 ? 50 : (int)((remaining + 999999) / 1000000);
        pollfd p = {xcb_get_file_descriptor(o->connection), POLLIN, 0};
        int result = poll(&p, 1, milliseconds);
        if ((result < 0 && errno != EINTR) || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            error = -EPIPE; break;
        }
    }
    pthread_mutex_unlock(&o->mutex); return error;
}
static int queue(hybris_icd_window *base, ANativeWindowBuffer *native, int fd) {
    int error = consume_fence(fd); if (error) return error;
    Owner *o = get(base);
    auto *b = static_cast<Buffer *>(static_cast<BaseNativeWindowBuffer *>(native));
    const native_handle_t *h = b->handle;
    if (!h || h->numFds < 0 || h->numFds > 64 || h->numInts < 0 || h->numInts > 1024) return -EINVAL;
    auto *p = static_cast<Pending *>(calloc(1, sizeof(Pending)));
    size_t bytes = sizeof(tawc_dri_present_buffer_req) + h->numInts * sizeof(int32_t);
    auto *data = static_cast<char *>(calloc(1, bytes));
    if (!p || !data) { free(p); free(data); return -ENOMEM; }
    int fds[64], copied = 0;
    for (; copied < h->numFds; ++copied) {
        fds[copied] = dup(h->data[copied]);
        if (fds[copied] < 0) {
            while (copied) close(fds[--copied]);
            free(p); free(data); return -errno;
        }
    }
    pthread_mutex_lock(&o->mutex);
    drain(o);
    do {
        if (!++o->serial) ++o->serial;
        p->serial = o->serial;
        for (Pending *q = o->pending; q; q = q->next) if (q->serial == p->serial) { p->serial = 0; break; }
    } while (!p->serial);
    auto *body = reinterpret_cast<tawc_dri_present_buffer_req *>(data);
    body->window = o->window; body->num_fds = h->numFds; body->num_ints = h->numInts;
    body->width = b->width; body->height = b->height; body->stride = b->stride;
    body->format = b->format; body->usage_lo = b->usage; body->serial = p->serial;
    memcpy(data + sizeof(*body), h->data + h->numFds, h->numInts * sizeof(int32_t));
    iovec parts[3] = {}; parts[2] = {data, bytes};
    xcb_protocol_request_t request = {1, &extension, X_TAWCDRI_PresentBuffer, 1};
    unsigned sequence = xcb_send_request_with_fds(o->connection, XCB_REQUEST_CHECKED,
        parts + 2, &request, h->numFds, fds);
    /* libxcb owns the duplicate FDs after send_request_with_fds. */
    error = check(o->connection, sequence);
    if (!error) {
        p->buffer = b; b->common.incRef(&b->common); b->held = true;
        p->next = o->pending; o->pending = p;
        trace(o, "present", p);
    } else free(p);
    free(data); pthread_mutex_unlock(&o->mutex); return error;
}
static int cancel(hybris_icd_window *base, ANativeWindowBuffer *native, int fd) {
    int error = consume_fence(fd); if (error) return error;
    Owner *o = get(base); pthread_mutex_lock(&o->mutex);
    for (unsigned i = 0; i < o->count; ++i) if (static_cast<ANativeWindowBuffer *>(o->pool[i]) == native) o->pool[i]->held = false;
    pthread_mutex_unlock(&o->mutex); return 0;
}
static int extent(hybris_icd_window *base, uint32_t *width, uint32_t *height) {
    Owner *o = get(base); pthread_mutex_lock(&o->mutex);
    xcb_generic_error_t *error = nullptr;
    auto *geometry = xcb_get_geometry_reply(o->connection, xcb_get_geometry(o->connection, o->window), &error);
    int result = !geometry || error ? -EPIPE : 0;
    if (!result) { *width = geometry->width; *height = geometry->height; }
    free(geometry); free(error); pthread_mutex_unlock(&o->mutex); return result;
}
static void destroy(hybris_icd_window *base) {
    Owner *o = get(base);
    select(o, 0); xcb_unregister_for_special_event(o->connection, o->events);
    drop_pool(o);
    while (o->pending) {
        Pending *p = o->pending; o->pending = p->next;
        p->buffer->common.decRef(&p->buffer->common); free(p);
    }
    pthread_mutex_destroy(&o->mutex); free(o);
}
static const hybris_icd_window_ops ops = {configure, dequeue, queue, cancel, disconnect, destroy, extent};
int hybris_icd_xcb_supported(xcb_connection_t *connection, uint32_t visual) {
    if (!connection || xcb_connection_has_error(connection)) return 0;
    sockaddr_storage peer = {}; socklen_t peer_size = sizeof(peer);
    if (getpeername(xcb_get_file_descriptor(connection), reinterpret_cast<sockaddr *>(&peer), &peer_size) || peer.ss_family != AF_UNIX) return 0;
    bool compatible = false;
    const xcb_setup_t *setup = xcb_get_setup(connection);
    for (auto screen = xcb_setup_roots_iterator(setup); screen.rem; xcb_screen_next(&screen))
        for (auto depth = xcb_screen_allowed_depths_iterator(screen.data); depth.rem; xcb_depth_next(&depth))
            for (auto v = xcb_depth_visuals_iterator(depth.data); v.rem; xcb_visualtype_next(&v))
                if (v.data->visual_id == visual && v.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR &&
                    (depth.data->depth == 24 || depth.data->depth == 32) &&
                    v.data->red_mask == 0xff0000 && v.data->green_mask == 0xff00 && v.data->blue_mask == 0xff) compatible = true;
    const xcb_query_extension_reply_t *present = xcb_get_extension_data(connection, &extension);
    if (!compatible || !present || !present->present) return 0;
    tawc_dri_query_version_req body = {}; body.major_version = 0; body.minor_version = 3;
    iovec parts[3] = {}; parts[2] = {&body, sizeof(body)};
    xcb_protocol_request_t request = {1, &extension, X_TAWCDRI_QueryVersion, 0};
    unsigned sequence = xcb_send_request(connection, XCB_REQUEST_CHECKED, parts + 2, &request);
    xcb_generic_error_t *error = nullptr;
    auto *reply = static_cast<tawc_dri_query_version_reply *>(xcb_wait_for_reply(connection, sequence, &error));
    bool ok = reply && !error && reply->major_version == 0 && reply->minor_version >= 3;
    free(reply); free(error); return ok;
}
int hybris_icd_window_xcb(xcb_connection_t *connection, uint32_t window, hybris_icd_window **out) {
    xcb_generic_error_t *error = nullptr;
    auto *attributes = xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), &error);
    if (!attributes || error) { free(attributes); free(error); return -EPIPE; }
    bool supported = hybris_icd_xcb_supported(connection, attributes->visual);
    free(attributes);
    if (!supported) return -ENOTSUP;
    auto *o = static_cast<Owner *>(calloc(1, sizeof(Owner)));
    if (!o) return -ENOMEM;
    const char *logging = getenv("HYBRIS_X11_TRACE");
    o->trace = logging && !strcmp(logging, "1");
    o->base.ops = &ops; o->connection = connection; o->window = window; o->eid = xcb_generate_id(connection);
    pthread_mutex_init(&o->mutex, nullptr);
    o->events = xcb_register_for_special_xge(connection, &extension, o->eid, nullptr);
    if (!o->events) { pthread_mutex_destroy(&o->mutex); free(o); return -ENOMEM; }
    int result = select(o, TAWC_DRI_EVENT_MASK_CONFIGURE_NOTIFY | TAWC_DRI_EVENT_MASK_BUFFER_RELEASE);
    if (result) { destroy(&o->base); return result; }
    *out = &o->base; return 0;
}
