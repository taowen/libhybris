/*
 * Copyright (c) 2012 Carsten Munk <carsten.munk@gmail.com>
 * Copyright (c) 2012 Canonical Ltd
 * Copyright (c) 2013 Christophe Chapuis <chris.chapuis@gmail.com>
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

#include "config.h"
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logging.h"
#include "bionic_tls.h"

#define TRACE_HOOK(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

/* Bionic TLS compat for stock (unpatched) Android firmware.
 *
 * Layout, per thread (zero-initialised by glibc TLS image init):
 *
 *   tls_static_tls[0..7]    = TLS_SLOT_BIONIC_TLS (-> bionic_tls_ptr),
 *                              the lone bionic negative slot in this Q
 *                              vendor (MIN_TLS_SLOT = -1, see
 *                              q/bionic/libc/private/bionic_asm_tls.h).
 *   tls_static_tls[8]        = bionic THREAD POINTER (= "TPIDR" from bionic's POV)
 *                              [8..15]  = slot 0 (TLS_SLOT_DTV)
 *                              [16..23] = slot 1 (TLS_SLOT_THREAD_ID)
 *                              ...
 *                              [64..71] = slot 7 (end of bionic_tcb positive slots)
 *   tls_static_tls[72..]     = per-.so static TLS data (.tdata copied here
 *                              from promote_tls_module_to_static() at the
 *                              offsets the bionic linker assigns)
 *
 * BIONIC_TPIDR_OFFSET (8) and BIONIC_TLS_PTR_OFFSET (0) are calibrated
 * to bionic Q's StaticTlsLayout: offset_bionic_tcb_ = 0 after the
 * initial reserve_exe_segment_and_tcb(nullptr,...), and
 * offset_thread_pointer() = offset_bionic_tcb_ + (-MIN_TLS_SLOT * 8) =
 * 0 + 1*8 = 8. linker.cpp's tls_tp_base reads offset_thread_pointer()
 * dynamically, so it stays in sync if a future bionic vendor changes
 * MIN_TLS_SLOT (e.g. R+ adds TLS_SLOT_NATIVE_BRIDGE_GUEST_STATE = -2,
 * pushing TP to offset 16).
 *
 * The TLS thunk patcher (tls_patcher_aarch64.c) rewrites bionic code's
 * `mrs xN, tpidr_el0` to point at &tls_static_tls[BIONIC_TPIDR_OFFSET],
 * so a bionic instruction `[TPIDR + disp]` (disp = static_offset -
 * tls_tp_base) reads tls_static_tls[8 + disp] = tls_static_tls[
 * static_offset], where this hook memcpy'd the .tdata.
 *
 * Size constraint: this whole array lives in the static-TLS *initial-exec*
 * block (required so the patcher can compute a fixed tp-relative offset).
 * For the main exe the kernel allocates whatever the link map needs; for
 * libhybris dlopened mid-process (GTK/firefox loading libEGL.so.1 ->
 * libhybris-common.so.1) the new IE bytes have to fit into glibc's
 * static_tls_surplus reserve, which varies per process based on what IE
 * TLS the binary's DT_NEEDED libs already pulled in. firefox's link map
 * is fatter than gtk3-demo-application's: 1536 bytes loads cleanly into
 * gtk3 but blows firefox's surplus ("cannot allocate memory in static
 * TLS block" on dlopen of libhybris-common). 1024 bytes (matching the
 * original libhybris tls_area struct of `void* + void*[128]`) loads
 * into every process we test on aarch64 + Arch ARM glibc 2.41, so we
 * keep that as the ceiling here.
 *
 * Cost: the 952 bytes left after subtracting bionic_tcb (72B = 9 slots
 * * 8) is the total per-process budget for promoted bionic .so .tdata.
 * apex libc + the Adreno/Mesa/Vulkan vendor TLS in our integration
 * suite fits well inside that. The libhybris-tls-repro stress test
 * deliberately leaks per dlopen+dlclose iteration (libhybris's promoted
 * static_tls slots can't be recycled without risking a static-pointer
 * free in the dynamic-tls path -- see linker_tls.cpp::get_unused_module_index),
 * so the test caps at STRESS_ITERATIONS to fit. If a real future
 * bionic stack actually needs more, the right move is to make
 * tls_static_tls a small IE-allocated `char* heap_block` and have the
 * patcher emit one extra LDR in the thunk to dereference it -- that
 * frees us from the IE-surplus cap entirely, at the cost of growing
 * the thunk from 16 to 20 bytes and matching count_tls/THUNK_SIZE.
 */
#define BIONIC_STATIC_TLS_SIZE   1024
#define BIONIC_TPIDR_OFFSET      8    /* matches StaticTlsLayout::offset_thread_pointer() with MIN_TLS_SLOT=-1 (bionic Q) */
#define BIONIC_TLS_PTR_OFFSET    0    /* slot -1 (TLS_SLOT_BIONIC_TLS) at TP-8 = 0 */
#define BIONIC_TLS_COMPAT_SIZE   16384
/* slot 1 (TLS_SLOT_THREAD_ID) at TP+8: bionic libc dereferences this
 * as a pthread_internal_t* in every syscall wrapper's errno-set path
 * (__set_errno_internal does `str w9, [x8, #776]` after loading slot 1,
 * where 776 = pthread_internal_t::errno_value), and in __cxa_thread_*
 * for per-thread atexit lists. Without a populated shadow, vendors
 * whose code paths hit a libc syscall failure (e.g. Pixel's
 * mapper.pixel.so mmap'ing a gralloc buffer fd) NULL-deref at +0x308.
 * Shadow size chosen to comfortably cover Q's pthread_internal_t
 * (~2 KiB) plus headroom for future bionic versions that grow it. */
#define BIONIC_THREAD_ID_PTR_OFFSET  (BIONIC_TPIDR_OFFSET + 8)  /* TP+8 = 16 */
#define BIONIC_PTHREAD_SHADOW_SIZE   8192

static pthread_key_t bionic_tls_cleanup_key;
static pthread_key_t pthread_shadow_cleanup_key;
static pthread_once_t bionic_tls_key_once = PTHREAD_ONCE_INIT;

static void _bionic_tls_cleanup(void *ptr)
{
    free(ptr);
}

static void _bionic_tls_key_init(void)
{
    int error = pthread_key_create(&bionic_tls_cleanup_key, _bionic_tls_cleanup);
    if (!error) error = pthread_key_create(&pthread_shadow_cleanup_key, _bionic_tls_cleanup);
    if (error) {
        fprintf(stderr, "HYBRIS: fatal: TLS cleanup key creation failed (%d)\n", error);
        abort();
    }
}

static __attribute__((tls_model ("initial-exec"), aligned(16)))
       __thread char tls_static_tls[BIONIC_STATIC_TLS_SIZE];
/* Per-thread "how many promoted-TLS-module entries from the global
 * registry have been replayed into this thread's tls_static_tls".
 * Compared against g_promoted_tls.count on first bionic-side touch
 * (_hybris_hook___get_tls_hooks) to catch up modules promoted before
 * this thread first ran any bionic code. */
static __attribute__((tls_model ("initial-exec")))
       __thread int  tls_inited;
static __attribute__((tls_model ("initial-exec")))
       __thread int  tls_module_init_count;

#define bionic_tls_ptr (*(void**)(tls_static_tls + BIONIC_TLS_PTR_OFFSET))
#define pthread_internal_shadow_ptr \
    (*(void**)(tls_static_tls + BIONIC_THREAD_ID_PTR_OFFSET))

/* Registry of every TLS-using bionic .so the linker has promoted to a
 * static-TLS slot. linker_tls.cpp's promote_tls_module_to_static() calls
 * _hybris_init_static_tls_for_thread() under the libc tls_modules
 * rwlock, which both appends an entry here and copies .tdata into the
 * *promoting* thread's tls_static_tls. For threads that are NOT the
 * promoter (notably glibc-spawned worker threads that later first-touch
 * bionic state), _hybris_hook___get_tls_hooks() walks this registry
 * once-per-thread to replay every promote we did before they existed.
 *
 * `init_data` is hybris-owned: we malloc + memcpy the .tdata bytes off
 * the .so segment at promote time. That outlives hybris_dlclose(),
 * which the post-dlclose-replay regression test exercises directly --
 * a thread first-touching bionic state after the .so has unmapped will
 * read these bytes, not the freed segment.
 *
 * Append-only: dlclose of a TLS-using bionic .so leaves the entry
 * orphaned (its `init_data`, `static_offset` etc all stay valid). Slot
 * leak is the same as the pre-existing static_tls_layout offset leak,
 * bounded by the count of distinct IE/TLSDESC-using bionic libs ever
 * loaded. */
struct hybris_promoted_tls {
    size_t static_offset;
    void*  init_data;     /* hybris-owned copy of .tdata, NULL if init_size==0 */
    size_t init_size;     /* filesz: bytes to memcpy into static-TLS */
    size_t segment_size;  /* memsz: total reserved range; .tbss tail beyond
                           * init_size stays as the calling thread's existing
                           * tls_static_tls bytes (zero-init from glibc TLS
                           * image init -- safe because reserve_solib_segment
                           * is monotonic and never overlaps with prior memsz) */
};

static struct {
    struct hybris_promoted_tls* entries;
    int count;
    int capacity;
    pthread_mutex_t mutex;
} g_promoted_tls = { NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER };

static void hybris_apply_static_tls_locked(const struct hybris_promoted_tls* e)
{
    /* memsz can exceed filesz when the .so has .tbss; bound on the full
     * segment so a future bionic .so whose .tbss tail would land past
     * BIONIC_STATIC_TLS_SIZE aborts here, instead of corrupting whatever
     * IE TLS the host glibc allocated next to tls_static_tls. */
    if (e->static_offset > BIONIC_STATIC_TLS_SIZE ||
        e->segment_size > BIONIC_STATIC_TLS_SIZE - e->static_offset ||
        e->init_size > e->segment_size) {
        fprintf(stderr, "HYBRIS: fatal: bionic static TLS overflow "
                        "(offset=%zu memsz=%zu, max=%d). Bump BIONIC_STATIC_TLS_SIZE in bionic_tls.c.\n",
                e->static_offset, e->segment_size, BIONIC_STATIC_TLS_SIZE);
        abort();
    }
    if (e->init_size > 0) {
        memcpy(tls_static_tls + e->static_offset, e->init_data, e->init_size);
    }
}

/* Linker -> hooks callback: record a promoted TLS module AND copy its
 * .tdata into the calling thread's tls_static_tls. The caller (the
 * bionic linker, in promote_tls_module_to_static) already holds the
 * libc tls_modules rwlock; init_ptr is only valid for the duration of
 * this call (the source .so may be hybris_dlclose'd later), so we
 * memdup before stashing in the registry. */
__attribute__((__visibility__("default")))
void _hybris_init_static_tls_for_thread(size_t static_offset,
                                         const void* init_ptr,
                                         size_t init_size,
                                         size_t segment_size)
{
    /* Validate before allocation, source reads or publication to the registry.
     * Subtraction avoids wrapping a malicious/corrupt offset + memsz. */
    if (static_offset > BIONIC_STATIC_TLS_SIZE ||
        segment_size > BIONIC_STATIC_TLS_SIZE - static_offset ||
        init_size > segment_size || (init_size && !init_ptr)) {
        fprintf(stderr, "HYBRIS: fatal: invalid promoted TLS range "
                        "(offset=%zu filesz=%zu memsz=%zu)\n",
                static_offset, init_size, segment_size);
        abort();
    }
    void* owned_init = NULL;
    if (init_size > 0) {
        owned_init = malloc(init_size);
        if (!owned_init) {
            fprintf(stderr, "HYBRIS: fatal: out of memory copying promoted-TLS .tdata (%zu bytes)\n",
                    init_size);
            abort();
        }
        memcpy(owned_init, init_ptr, init_size);
    }
    pthread_mutex_lock(&g_promoted_tls.mutex);
    if (g_promoted_tls.count == g_promoted_tls.capacity) {
        int new_cap = g_promoted_tls.capacity == 0 ? 16 : g_promoted_tls.capacity * 2;
        struct hybris_promoted_tls* grown = realloc(g_promoted_tls.entries,
                                                    new_cap * sizeof(*grown));
        if (!grown) {
            fprintf(stderr, "HYBRIS: fatal: out of memory growing promoted-TLS registry\n");
            abort();
        }
        g_promoted_tls.entries = grown;
        g_promoted_tls.capacity = new_cap;
    }
    struct hybris_promoted_tls* e = &g_promoted_tls.entries[g_promoted_tls.count++];
    e->static_offset = static_offset;
    e->init_data     = owned_init;
    e->init_size     = init_size;
    e->segment_size  = segment_size;
    /* The promoter may not have touched TLS since another thread appended
     * modules. Apply that missing prefix too before advancing its cursor.
     * Never replay earlier entries: they may contain live thread-local writes. */
    for (int i = tls_module_init_count; i < g_promoted_tls.count; i++) {
        hybris_apply_static_tls_locked(&g_promoted_tls.entries[i]);
    }
    tls_module_init_count = g_promoted_tls.count;
    pthread_mutex_unlock(&g_promoted_tls.mutex);
}

__attribute__((__visibility__("default")))
void *_hybris_hook___get_tls_hooks()
{
    TRACE_HOOK("");

    /* Lazily allocate a bionic_tls struct for this thread, and replay
     * any TLS-module .tdata that was promoted to static before this
     * thread first touched bionic-side state. Modules promoted later
     * land in this thread only if it happens to be the dlopening
     * thread (linker_tls.cpp -> _hybris_init_static_tls_for_thread on
     * the promoter); other threads see zeros for those modules until
     * they next call back through this function. */
    if (__builtin_expect(!tls_inited, 0)) {
        void *btls = calloc(1, BIONIC_TLS_COMPAT_SIZE);
        if (!btls) {
            fprintf(stderr, "HYBRIS: fatal: failed to allocate bionic_tls compat struct\n");
            abort();
        }
        bionic_tls_ptr = btls;

        /* slot 1 (TLS_SLOT_THREAD_ID) shadow: zero-init'd buffer the
         * bionic libc syscall wrappers and __cxa_thread_* helpers
         * dereference. See BIONIC_THREAD_ID_PTR_OFFSET above for the
         * mechanism. Vendors whose code paths set errno (e.g. Pixel's
         * mapper.pixel.so on Tensor G5) crash without this. */
        void *pthread_shadow = calloc(1, BIONIC_PTHREAD_SHADOW_SIZE);
        if (!pthread_shadow) {
            fprintf(stderr, "HYBRIS: fatal: failed to allocate pthread_internal_t shadow\n");
            abort();
        }
        /* Q and current bionic pthread_internal_t start with next/prev then
         * pid_t tid. Inlined bionic mutex code reads this prefix directly. */
        pid_t tid = (pid_t)syscall(SYS_gettid);
        memcpy((char *)pthread_shadow + 2 * sizeof(void *), &tid, sizeof(tid));
        pthread_internal_shadow_ptr = pthread_shadow;

        pthread_once(&bionic_tls_key_once, _bionic_tls_key_init);
        int error = pthread_setspecific(bionic_tls_cleanup_key, btls);
        if (!error) error = pthread_setspecific(pthread_shadow_cleanup_key, pthread_shadow);
        if (error) {
            fprintf(stderr, "HYBRIS: fatal: TLS cleanup registration failed (%d)\n", error);
            abort();
        }
        tls_inited = 1;
    }

    /* Catch up from any promote that happened on another thread since
     * we last ran. Done outside the !tls_inited gate so a thread that's
     * already inited still picks up newly-promoted modules on its next
     * hooked-libc call.
     *
     * Take the mutex before reading .count: the writer
     * (_hybris_init_static_tls_for_thread) mutates count under the same
     * lock, and an unsynchronised read here is a C data race. Reads of
     * tls_module_init_count itself need no lock -- it's a per-thread
     * __thread int with single-writer (this thread).
     *
     * Q's static TLSDESC resolver also calls this hook before returning
     * the offset, so pure TLSDESC first touches catch up without a libc
     * call. IE accesses that bypass that resolver still require an
     * initialized thread. */
    pthread_mutex_lock(&g_promoted_tls.mutex);
    if (__builtin_expect(tls_module_init_count < g_promoted_tls.count, 0)) {
        for (int i = tls_module_init_count; i < g_promoted_tls.count; i++) {
            hybris_apply_static_tls_locked(&g_promoted_tls.entries[i]);
        }
        tls_module_init_count = g_promoted_tls.count;
    }
    pthread_mutex_unlock(&g_promoted_tls.mutex);

    return tls_static_tls + BIONIC_TPIDR_OFFSET;
}

/* Called only by the first-touch assembly helper. MRS cannot change errno. */
__attribute__((visibility("hidden")))
void hybris_tls_first_touch_c(void)
{
    int saved_errno = errno;
    _hybris_hook___get_tls_hooks();
    errno = saved_errno;
}
