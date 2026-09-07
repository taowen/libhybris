/*
 * Copyright (c) 2018-2022 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef ANDROID_BUILD
#include <android-config.h>
#endif
#include <stdlib.h>

#include <hardware/hardware.h>

#include <hardware/gralloc.h>
#if HAS_GRALLOC1_HEADER
#include <hybris/grallocusage/GrallocUsageConversion.h>
#include <hardware/gralloc1.h>
#endif
#include <hardware/fb.h>

#ifdef ANDROID_BUILD
#include "hybris-gralloc.h"
#else
#include <hybris/gralloc/gralloc.h>
#include <hybris/ui/ui.h>
#include <hybris/common/binding.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

#include <dlfcn.h>
#include "logging.h"

// AHardwareBuffer backend. Public NDK API backed by libnativewindow.so, which
// dispatches to whichever gralloc the system currently ships (gralloc4 on
// modern Android). Using AHB gives us handle layouts that match what the
// Android-side mapper expects, fixing cross-process buffer sharing on stock
// firmware where GRALLOC1 / GRALLOC0 would produce handles the current
// mapper rejects.
#include <android/hardware_buffer.h>

static int version = -1;
static hw_module_t *gralloc_hardware_module = NULL;

static framebuffer_device_t *framebuffer_device = NULL;
static gralloc_module_t *gralloc0_module;
static alloc_device_t *gralloc0_alloc;

#if HAS_GRALLOC1_HEADER
static gralloc1_device_t *gralloc1_device = NULL;
static int gralloc1_release_implies_delete = 0;
static GRALLOC1_PFN_CREATE_DESCRIPTOR gralloc1_create_descriptor = NULL;
static GRALLOC1_PFN_DESTROY_DESCRIPTOR gralloc1_destroy_descriptor = NULL;
static GRALLOC1_PFN_SET_CONSUMER_USAGE gralloc1_set_consumer_usage = NULL;
static GRALLOC1_PFN_SET_DIMENSIONS gralloc1_set_dimensions = NULL;
static GRALLOC1_PFN_SET_FORMAT gralloc1_set_format = NULL;
static GRALLOC1_PFN_SET_PRODUCER_USAGE gralloc1_set_producer_usage = NULL;
static GRALLOC1_PFN_GET_BACKING_STORE gralloc1_get_backing_store = NULL;
static GRALLOC1_PFN_GET_CONSUMER_USAGE gralloc1_get_consumer_usage = NULL;
static GRALLOC1_PFN_GET_DIMENSIONS gralloc1_get_dimensions = NULL;
static GRALLOC1_PFN_GET_FORMAT gralloc1_get_format = NULL;
static GRALLOC1_PFN_GET_PRODUCER_USAGE gralloc1_get_producer_usage = NULL;
static GRALLOC1_PFN_GET_STRIDE gralloc1_get_stride = NULL;
static GRALLOC1_PFN_ALLOCATE gralloc1_allocate = NULL;
static GRALLOC1_PFN_RETAIN gralloc1_retain = NULL;
static GRALLOC1_PFN_RELEASE gralloc1_release = NULL;
static GRALLOC1_PFN_GET_NUM_FLEX_PLANES gralloc1_get_num_flex_planes = NULL;
static GRALLOC1_PFN_LOCK gralloc1_lock = NULL;
static GRALLOC1_PFN_LOCK_FLEX gralloc1_lock_flex = NULL;
static GRALLOC1_PFN_UNLOCK gralloc1_unlock = NULL;
#ifdef GRALLOC1_PFN_SET_LAYER_COUNT
static GRALLOC1_PFN_SET_LAYER_COUNT gralloc1_set_layer_count = NULL;
static GRALLOC1_PFN_GET_LAYER_COUNT gralloc1_get_layer_count = NULL;
#endif

static void gralloc1_init(void);
#endif

// ---------------------------------------------------------------------------
// AHB backend state (version == 3)
// ---------------------------------------------------------------------------

typedef int    (*ahb_allocate_fn)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
typedef void   (*ahb_describe_fn)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
typedef int    (*ahb_lock_fn)(AHardwareBuffer *, uint64_t, int32_t, const ARect *, void **);
typedef int    (*ahb_unlock_fn)(AHardwareBuffer *, int32_t *);
typedef void   (*ahb_acquire_fn)(AHardwareBuffer *);
typedef void   (*ahb_release_fn)(AHardwareBuffer *);
typedef const native_handle_t * (*ahb_get_native_handle_fn)(const AHardwareBuffer *);
typedef int    (*ahb_create_from_handle_fn)(const AHardwareBuffer_Desc *, const native_handle_t *, int32_t, AHardwareBuffer **);

static void *ahb_libnativewindow = NULL;
static ahb_allocate_fn           ahb_allocate_sym;
static ahb_describe_fn           ahb_describe_sym;
static ahb_lock_fn               ahb_lock_sym;
static ahb_unlock_fn             ahb_unlock_sym;
static ahb_acquire_fn            ahb_acquire_sym;
static ahb_release_fn            ahb_release_sym;
static ahb_get_native_handle_fn  ahb_get_native_handle_sym;
static ahb_create_from_handle_fn ahb_create_from_handle_sym;

// AHB method values for AHardwareBuffer_createFromHandle. Not in the public
// NDK header; cribbed from AOSP frameworks/native/libs/nativewindow/include/
// private/android/AHardwareBufferHelpers.h.
#define AHB_METHOD_CLONE    1
#define AHB_METHOD_REGISTER 2

// Map native_handle_t* (returned by AHardwareBuffer_getNativeHandle) -> AHB
// pointer + shadow refcount. AHB's own refcount is opaque, so we track it
// ourselves to know when to remove the map entry.
typedef struct ahb_entry {
    const native_handle_t *handle;
    AHardwareBuffer       *ahb;
    int                    refcount;
    struct ahb_entry      *next;
} ahb_entry_t;

static ahb_entry_t    *ahb_map_head = NULL;
static pthread_mutex_t ahb_map_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ahb_map_insert(const native_handle_t *handle, AHardwareBuffer *ahb) {
    ahb_entry_t *e = (ahb_entry_t *)malloc(sizeof(*e));
    if (!e) return;
    e->handle   = handle;
    e->ahb      = ahb;
    e->refcount = 1;
    pthread_mutex_lock(&ahb_map_mutex);
    e->next = ahb_map_head;
    ahb_map_head = e;
    pthread_mutex_unlock(&ahb_map_mutex);
}

static AHardwareBuffer *ahb_map_find(const native_handle_t *handle) {
    AHardwareBuffer *ahb = NULL;
    pthread_mutex_lock(&ahb_map_mutex);
    for (ahb_entry_t *e = ahb_map_head; e; e = e->next) {
        if (e->handle == handle) { ahb = e->ahb; break; }
    }
    pthread_mutex_unlock(&ahb_map_mutex);
    return ahb;
}

struct AHardwareBuffer *hybris_gralloc_get_hardware_buffer(buffer_handle_t handle) {
    return ahb_map_find(handle);
}

static int ahb_map_incref(const native_handle_t *handle) {
    int rc = -ENOENT;
    pthread_mutex_lock(&ahb_map_mutex);
    for (ahb_entry_t *e = ahb_map_head; e; e = e->next) {
        if (e->handle == handle) { e->refcount++; rc = 0; break; }
    }
    pthread_mutex_unlock(&ahb_map_mutex);
    return rc;
}

// Decrement the shadow refcount.
// Returns 0 and sets *out_ahb when the refcount hits zero (caller must
// AHardwareBuffer_release), 1 when refs remain, or -ENOENT when the
// handle is not in the map at all.
static int ahb_map_decref_take(const native_handle_t *handle, AHardwareBuffer **out_ahb) {
    int result = -ENOENT;
    *out_ahb = NULL;
    pthread_mutex_lock(&ahb_map_mutex);
    ahb_entry_t **prev = &ahb_map_head;
    for (ahb_entry_t *e = *prev; e; prev = &e->next, e = *prev) {
        if (e->handle == handle) {
            if (--e->refcount <= 0) {
                *out_ahb = e->ahb;
                *prev = e->next;
                free(e);
                result = 0;
            } else {
                result = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&ahb_map_mutex);
    return result;
}

// Best-effort load of libnativewindow.so via the bionic linker. Returns 1 on
// success, 0 on failure. On failure, the caller falls through to the
// GRALLOC_COMPAT / GRALLOC1 / GRALLOC0 init chain.
static int ahb_init(void) {
#ifdef ANDROID_BUILD
    ahb_libnativewindow = dlopen("libnativewindow.so", RTLD_NOW);
#else
    ahb_libnativewindow = android_dlopen("libnativewindow.so", RTLD_NOW);
#endif
    if (!ahb_libnativewindow) {
        return 0;
    }

#ifdef ANDROID_BUILD
#define AHB_DLSYM(sym) dlsym(ahb_libnativewindow, sym)
#else
#define AHB_DLSYM(sym) android_dlsym(ahb_libnativewindow, sym)
#endif
    ahb_allocate_sym           = (ahb_allocate_fn)          AHB_DLSYM("AHardwareBuffer_allocate");
    ahb_describe_sym           = (ahb_describe_fn)          AHB_DLSYM("AHardwareBuffer_describe");
    ahb_lock_sym               = (ahb_lock_fn)              AHB_DLSYM("AHardwareBuffer_lock");
    ahb_unlock_sym             = (ahb_unlock_fn)            AHB_DLSYM("AHardwareBuffer_unlock");
    ahb_acquire_sym            = (ahb_acquire_fn)           AHB_DLSYM("AHardwareBuffer_acquire");
    ahb_release_sym            = (ahb_release_fn)           AHB_DLSYM("AHardwareBuffer_release");
    ahb_get_native_handle_sym  = (ahb_get_native_handle_fn) AHB_DLSYM("AHardwareBuffer_getNativeHandle");
    ahb_create_from_handle_sym = (ahb_create_from_handle_fn)AHB_DLSYM("AHardwareBuffer_createFromHandle");
#undef AHB_DLSYM

    if (!ahb_allocate_sym || !ahb_describe_sym || !ahb_lock_sym ||
        !ahb_unlock_sym   || !ahb_acquire_sym  || !ahb_release_sym ||
        !ahb_get_native_handle_sym || !ahb_create_from_handle_sym) {
        ahb_libnativewindow = NULL;
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------

// simple macros to make sure the code is only compiled if we actually have the
// header to be able to compile it.
// we could also use Gralloc1On0Adapter, but that would mean we need to import
// headers and cpp files from android 8, which may not compile against older
// android trees.
#if HAS_GRALLOC1_HEADER
#define GRALLOC0(code) (version == 0) { code }
#define GRALLOC1(code) (version == 1) { code }
#else
#define GRALLOC0(code) (version == 0) { code }
#define GRALLOC1(code) (0) {}
#endif

#if ANDROID_VERSION_MAJOR>=10
#define GRALLOC_COMPAT(code) (version == 2) { code }
#else
#define GRALLOC_COMPAT(code) (0) {}
#endif

// AHardwareBuffer-based backend, preferred when libnativewindow.so loads.
#define GRALLOC_AHB(code) (version == 3) { code }

#define NO_GRALLOC { fprintf(stderr, "%s:%d: called gralloc method without gralloc loaded\n", __func__, __LINE__); assert(NULL); }

void hybris_gralloc_deinitialize(void);

void hybris_gralloc_initialize(int framebuffer)
{
    if (version == -1) {
        // Prefer the AHB backend: it talks to whatever gralloc the system
        // currently ships (gralloc4 on modern Android) via the public NDK
        // API, so handles are always in a format the Android-side mapper
        // understands. The framebuffer path still needs GRALLOC0 because AHB
        // doesn't expose framebuffer_device_t operations.
        if (!framebuffer && ahb_init()) {
            version = 3;
            atexit(hybris_gralloc_deinitialize);
        } else
#if ANDROID_VERSION_MAJOR>=10
        if (hybris_ui_initialize(), hybris_ui_check_for_symbol("graphic_buffer_allocator_allocate")) {
            version = 2;
        } else
#endif
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const struct hw_module_t **)&gralloc_hardware_module) == 0) {
#if HAS_GRALLOC1_HEADER
            uint8_t majorVersion = (gralloc_hardware_module->module_api_version >> 8) & 0xFF;
            if ((majorVersion == 1) && (gralloc1_open(gralloc_hardware_module, &gralloc1_device) == 0) && (gralloc1_device != NULL)) {
                // success
                gralloc1_init();
                version = 1;
                atexit(hybris_gralloc_deinitialize);
            } else
#endif
            if (framebuffer) {
                if (framebuffer_open(gralloc_hardware_module, &framebuffer_device) == 0) {
                    if ((gralloc_open(gralloc_hardware_module, &gralloc0_alloc) == 0) && gralloc0_alloc != NULL) {
                        // success
                        gralloc0_module = (struct gralloc_module_t*)gralloc_hardware_module;
                        version = 0;
                        atexit(hybris_gralloc_deinitialize);
                    } else {
                        fprintf(stderr, "failed to open the gralloc 0 module (framebuffer was requested therefore defaulted to version 0)\n");
                        assert(NULL);
                    }
                } else {
                    fprintf(stderr, "failed to open the framebuffer module\n");
                    assert(NULL);
                }
            } else
            if ((gralloc_open(gralloc_hardware_module, &gralloc0_alloc) == 0) && gralloc0_alloc != NULL) {
                // success
                gralloc0_module = (struct gralloc_module_t*)gralloc_hardware_module;
                version = 0;
                atexit(hybris_gralloc_deinitialize);
            } else {
                // fail
                framebuffer_device = NULL;
#if HAS_GRALLOC1_HEADER
                gralloc1_device = NULL;
#endif
                version = -2;
                fprintf(stderr, "failed to open gralloc module with both version 0 and 1 methods\n");
                hybris_gralloc_deinitialize();
                assert(NULL);
            }
        } else {
            fprintf(stderr, "failed to find/load gralloc module\n");
            assert(NULL);
        }
    } else {
        TRACE("hybris gralloc module has been already initialized\n");
    }
}

void hybris_gralloc_deinitialize(void)
{
    // AHB backend has no device/module to close — releases happen per-buffer.
    // The libnativewindow handle is process-global; leave it loaded.

    if (framebuffer_device) framebuffer_close(framebuffer_device);
    framebuffer_device = NULL;

    if (gralloc0_alloc) gralloc_close(gralloc0_alloc);
    gralloc0_alloc = NULL;

#if HAS_GRALLOC1_HEADER
    if (gralloc1_device) gralloc1_close(gralloc1_device);
    gralloc1_device = NULL;
#endif

#ifdef ANDROID_BUILD
    if (gralloc_hardware_module) dlclose(gralloc_hardware_module->dso);
#else
    if (gralloc_hardware_module) android_dlclose(gralloc_hardware_module->dso);
#endif
    gralloc_hardware_module = NULL;
}

#if HAS_GRALLOC1_HEADER
static void gralloc1_init(void)
{
    uint32_t count = 0;
    gralloc1_device->getCapabilities(gralloc1_device, &count, NULL);

    if (count >= 1) {
        int32_t i;
        int32_t *gralloc1_capabilities = (int32_t*)malloc(sizeof(int32_t) * count);

        gralloc1_device->getCapabilities(gralloc1_device, &count, gralloc1_capabilities);

        // currently the only one that affects us/interests us is release imply delete.
        for (i = 0; i < count; i++) {
#if ANDROID_VERSION_MAJOR >= 8
            if (gralloc1_capabilities[i] == GRALLOC1_CAPABILITY_RELEASE_IMPLY_DELETE) {
                gralloc1_release_implies_delete = 1;
            }
#endif
        }

        free(gralloc1_capabilities);
    }

    gralloc1_create_descriptor = (GRALLOC1_PFN_CREATE_DESCRIPTOR)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_CREATE_DESCRIPTOR);
    gralloc1_destroy_descriptor = (GRALLOC1_PFN_DESTROY_DESCRIPTOR)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_DESTROY_DESCRIPTOR);
    gralloc1_set_consumer_usage = (GRALLOC1_PFN_SET_CONSUMER_USAGE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_SET_CONSUMER_USAGE);
    gralloc1_set_dimensions = (GRALLOC1_PFN_SET_DIMENSIONS)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_SET_DIMENSIONS);
    gralloc1_set_format = (GRALLOC1_PFN_SET_FORMAT)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_SET_FORMAT);
    gralloc1_set_producer_usage = (GRALLOC1_PFN_SET_PRODUCER_USAGE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_SET_PRODUCER_USAGE);
    gralloc1_get_backing_store = (GRALLOC1_PFN_GET_BACKING_STORE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_BACKING_STORE);
    gralloc1_get_consumer_usage = (GRALLOC1_PFN_GET_CONSUMER_USAGE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_CONSUMER_USAGE);
    gralloc1_get_dimensions = (GRALLOC1_PFN_GET_DIMENSIONS)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_DIMENSIONS);
    gralloc1_get_format = (GRALLOC1_PFN_GET_FORMAT)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_FORMAT);
    gralloc1_get_producer_usage = (GRALLOC1_PFN_GET_PRODUCER_USAGE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_PRODUCER_USAGE);
    gralloc1_get_stride = (GRALLOC1_PFN_GET_STRIDE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_STRIDE);
    gralloc1_allocate = (GRALLOC1_PFN_ALLOCATE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_ALLOCATE);
    gralloc1_retain = (GRALLOC1_PFN_RETAIN)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_RETAIN);
    gralloc1_release = (GRALLOC1_PFN_RELEASE)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_RELEASE);
    gralloc1_get_num_flex_planes = (GRALLOC1_PFN_GET_NUM_FLEX_PLANES)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_NUM_FLEX_PLANES);
    gralloc1_lock = (GRALLOC1_PFN_LOCK)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_LOCK);
    gralloc1_lock_flex = (GRALLOC1_PFN_LOCK_FLEX)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_LOCK_FLEX);
    gralloc1_unlock = (GRALLOC1_PFN_UNLOCK)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_UNLOCK);
#ifdef GRALLOC1_PFN_SET_LAYER_COUNT
    gralloc1_set_layer_count = (GRALLOC1_PFN_SET_LAYER_COUNT)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_SET_LAYER_COUNT);
    gralloc1_get_layer_count = (GRALLOC1_PFN_GET_LAYER_COUNT)gralloc1_device->getFunction(gralloc1_device, GRALLOC1_FUNCTION_GET_LAYER_COUNT);
#endif
}
#endif

int hybris_gralloc_release(buffer_handle_t handle, int was_allocated)
{
    int ret = -ENOSYS;

    if GRALLOC_AHB(
        AHardwareBuffer *ahb = NULL;
        int rc = ahb_map_decref_take(handle, &ahb);
        if (rc == 0) {
            ahb_release_sym(ahb);
            ret = 0;
        } else if (rc == 1) {
            ret = 0;
        } else {
            fprintf(stderr, "hybris_gralloc_release: handle %p not in AHB map\n", handle);
            ret = -EINVAL;
        }
    ) else if GRALLOC_COMPAT(
        if (was_allocated) {
            ret = graphic_buffer_allocator_free(handle);
        } else {
            ret = graphic_buffer_mapper_free_buffer(handle);
        }
    ) else if GRALLOC1(
        ret = gralloc1_release(gralloc1_device, handle);

        // this needs to happen if the last reference is gone, this function is
        // only called in such cases.
        if (!gralloc1_release_implies_delete) {
            native_handle_close((native_handle_t*)handle);
            native_handle_delete((native_handle_t*)handle);
        }
    ) else if GRALLOC0(
        if (was_allocated) {
            ret = gralloc0_alloc->free(gralloc0_alloc, handle);
        } else {
            ret = gralloc0_module->unregisterBuffer(gralloc0_module, handle);

            // this needs to happen if the last reference is gone, this function is
            // only called in such cases.
            native_handle_close((native_handle_t*)handle);
            native_handle_delete((native_handle_t*)handle);
        }
    ) else NO_GRALLOC

    return ret;
}

int hybris_gralloc_import_buffer(buffer_handle_t raw_handle, buffer_handle_t* out_handle)
{
    int ret = -ENOSYS;

    if GRALLOC_AHB(
        // AHardwareBuffer_createFromHandle requires a fully-populated desc,
        // which this entry point doesn't carry. Callers that route through
        // the AHB backend need to use hybris_gralloc_allocate or the
        // matching server_wlegl path that knows the dimensions. Return
        // -ENOSYS so callers can detect the limitation.
        (void)raw_handle;
        (void)out_handle;
        ret = -ENOSYS;
    ) else if GRALLOC_COMPAT(
        ret = graphic_buffer_mapper_import_buffer_no_size(raw_handle, out_handle);
    ) else {
        // clone input buffer first when using gralloc 1 or 0
        // to keep same ownership semantics with HIDL HAL
        buffer_handle_t handle = NULL;
        handle = native_handle_clone((native_handle_t*)raw_handle);
        if (!handle)
            return -ENOSYS;

        ret = hybris_gralloc_retain(handle);
        *out_handle = handle;
    }

    return ret;
}

int hybris_gralloc_retain(buffer_handle_t handle)
{
    int ret = -ENOSYS;

    if GRALLOC_AHB(
        AHardwareBuffer *ahb = ahb_map_find(handle);
        if (ahb) {
            ahb_acquire_sym(ahb);
            ahb_map_incref(handle);
            ret = 0;
        } else {
            ret = -EINVAL;
        }
    ) else if GRALLOC1(
        ret = gralloc1_retain(gralloc1_device, handle);
    ) else if GRALLOC0(
        ret = gralloc0_module->registerBuffer(gralloc0_module, handle);
    ) else NO_GRALLOC

    return ret;
}

int hybris_gralloc_allocate(int width, int height, int format, int usage, buffer_handle_t *handle_ptr, uint32_t *stride_ptr)
{
    int ret = -ENOSYS;

    if GRALLOC_AHB(
        // AHB format constants match HAL_PIXEL_FORMAT_* for the common cases
        // (RGBA_8888, RGB_888, RGB_565, RGBA_FP16, RGBA_1010102). AHB usage
        // flags share the low bits with gralloc1 producer/consumer usage —
        // GPU_SAMPLED_IMAGE / GPU_COLOR_OUTPUT / COMPOSER_OVERLAY / CPU_*
        // align exactly, which covers everything libhybris callers ask for.
        AHardwareBuffer_Desc desc;
        desc.width  = (uint32_t)width;
        desc.height = (uint32_t)height;
        desc.layers = 1;
        desc.format = (uint32_t)format;
        desc.usage  = (uint64_t)(uint32_t)usage;
        desc.stride = 0;
        desc.rfu0   = 0;
        desc.rfu1   = 0;

        AHardwareBuffer *ahb = NULL;
        int rc = ahb_allocate_sym(&desc, &ahb);
        if (rc != 0 || !ahb) {
            ret = rc ? rc : -ENOMEM;
        } else {
            AHardwareBuffer_Desc actual;
            ahb_describe_sym(ahb, &actual);
            const native_handle_t *h = ahb_get_native_handle_sym(ahb);
            if (!h) {
                ahb_release_sym(ahb);
                ret = -EINVAL;
            } else {
                *handle_ptr = h;
                if (stride_ptr) *stride_ptr = actual.stride;
                ahb_map_insert(h, ahb);
                ret = 0;
            }
        }
    ) else if GRALLOC_COMPAT(
        ret = graphic_buffer_allocator_allocate(width, height, format, 1 /*layerCount*/, usage,
                                                handle_ptr, stride_ptr, 0 /*graphicBufferId*/, "hybris-gralloc");
    ) else if GRALLOC1(
        gralloc1_buffer_descriptor_t desc;
        uint64_t producer_usage;
        uint64_t consumer_usage;

        android_convertGralloc0To1Usage(usage, &producer_usage, &consumer_usage);

        // create temporary description (descriptor) of buffer to allocate
        ret = gralloc1_create_descriptor(gralloc1_device, &desc);
        ret |= gralloc1_set_dimensions(gralloc1_device, desc, width, height);
        ret |= gralloc1_set_consumer_usage(gralloc1_device, desc, consumer_usage);
        ret |= gralloc1_set_producer_usage(gralloc1_device, desc, producer_usage);
        ret |= gralloc1_set_format(gralloc1_device, desc, format);

        // actual allocation
        ret |= gralloc1_allocate(gralloc1_device, 1, &desc, handle_ptr);

        // get stride and release temporary descriptor
        ret |= gralloc1_get_stride(gralloc1_device, *handle_ptr, stride_ptr);
        ret |= gralloc1_destroy_descriptor(gralloc1_device, desc);
    ) else if GRALLOC0(
        ret = gralloc0_alloc->alloc(gralloc0_alloc,
                                    width, height, format, usage,
                                    handle_ptr, (int*)stride_ptr);
    ) else NO_GRALLOC

    return ret;
}

int hybris_gralloc_lock(buffer_handle_t handle, int usage, int l, int t, int w, int h, void **vaddr)
{
    int ret = -ENOSYS;

    if GRALLOC_AHB(
        AHardwareBuffer *ahb = ahb_map_find(handle);
        if (!ahb) {
            ret = -EINVAL;
        } else {
            ARect rect;
            rect.left   = l;
            rect.top    = t;
            rect.right  = l + w;
            rect.bottom = t + h;
            // Pass the full buffer when rect matches (left=0,top=0,w=0,h=0).
            const ARect *region = (l == 0 && t == 0 && w == 0 && h == 0)
                                      ? NULL : &rect;
            ret = ahb_lock_sym(ahb, (uint64_t)(uint32_t)usage, -1, region, vaddr);
        }
    ) else if GRALLOC_COMPAT(
        ARect bounds;
        int32_t outBytesPerPixel;
        int32_t outBytesPerStride;

        bounds.left = l;
        bounds.top = t;
        bounds.right = l + w;
        bounds.bottom = t + h;

        ret = graphic_buffer_mapper_lock(handle, usage, &bounds,
                                         vaddr, &outBytesPerPixel, &outBytesPerStride);
    ) else if GRALLOC1(
        uint64_t producer_usage;
        uint64_t consumer_usage;
        gralloc1_rect_t access_region;

        access_region.left = l;
        access_region.top = t;
        access_region.width = w;
        access_region.height = h;

        android_convertGralloc0To1Usage(usage, &producer_usage, &consumer_usage);

        ret = gralloc1_lock(gralloc1_device, handle, producer_usage, consumer_usage, &access_region, vaddr, -1);
    ) else if GRALLOC0(
        ret = gralloc0_module->lock(gralloc0_module, handle, usage, l, t, w, h, vaddr);
    ) else NO_GRALLOC

    return ret;
}

int hybris_gralloc_unlock(buffer_handle_t handle)
{
    int ret = -ENOSYS;

    if GRALLOC_AHB(
        AHardwareBuffer *ahb = ahb_map_find(handle);
        if (!ahb) {
            ret = -EINVAL;
        } else {
            int32_t fence = -1;
            ret = ahb_unlock_sym(ahb, &fence);
            if (fence >= 0) close(fence);
        }
    ) else if GRALLOC_COMPAT(
        ret = graphic_buffer_mapper_unlock(handle);
    ) else if GRALLOC1(
        int releaseFence = 0;
        ret = gralloc1_unlock(gralloc1_device, handle, &releaseFence);
        close(releaseFence);
    ) else if GRALLOC0(
        ret = gralloc0_module->unlock(gralloc0_module, handle);
    ) else NO_GRALLOC

    return ret;
}

// Legacy fbdev methods. these are not available in gralloc1 thus use old API.
int hybris_gralloc_fbdev_format(void)
{
    assert(framebuffer_device);
    return framebuffer_device->format;
}

int hybris_gralloc_fbdev_framebuffer_count(void)
{
    assert(framebuffer_device);
    return framebuffer_device->numFramebuffers;
}

int hybris_gralloc_fbdev_setSwapInterval(int interval)
{
    assert(framebuffer_device);
    return framebuffer_device->setSwapInterval(framebuffer_device, interval);
}

int hybris_gralloc_fbdev_post(buffer_handle_t handle)
{
    assert(framebuffer_device);
    return framebuffer_device->post(framebuffer_device, handle);
}

int hybris_gralloc_fbdev_width(void)
{
    assert(framebuffer_device);
    return framebuffer_device->width;
}

int hybris_gralloc_fbdev_height(void)
{
    assert(framebuffer_device);
    return framebuffer_device->height;
}
