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
#include <sys/mman.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logging.h"
#include "bionic_tls.h"

#define TRACE_HOOK(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

/* Bionic's simulated thread pointer has a process-independent offset in each
 * thread's private arena. Only the arena pointer and replay cursors use glibc
 * initial-exec TLS; Android module data must not consume glibc's small static-TLS
 * surplus when libhybris is dlopened into an existing process.
 *
 * Reserve virtual addresses once per thread and commit pages as the Android
 * linker promotes modules. The base never moves: native code can retain pointers
 * to earlier TLS variables while later dlopens extend the static layout.
 * Untouched pages have no physical backing. The reserve is an explicit bound
 * against corrupt module offsets, not a preallocated physical buffer.
 *
 * Q's bionic TCB occupies bytes [0,72): slot -1 points to bionic_tls, slot 0 is
 * the thread pointer at offset 8, and slot 1 points to the pthread shadow. Module
 * offsets come from the linker's StaticTlsLayout and are copied unchanged.
 * MRS thunks load the arena's thread pointer through its initial-exec pointer
 * slot; first touch initializes the TCB without clobbering registers or errno.
 */
#define BIONIC_TLS_RESERVE_SIZE  (16U * 1024U * 1024U)
#define BIONIC_TCB_SIZE          72U
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

static __attribute__((tls_model ("initial-exec")))
       __thread char *bionic_thread_pointer;
static __attribute__((tls_model ("initial-exec")))
       __thread size_t tls_mapped_bytes;
static __attribute__((tls_model ("initial-exec")))
       __thread int tls_inited;
static __attribute__((tls_model ("initial-exec")))
       __thread int tls_module_init_count;

#define tls_static_tls (bionic_thread_pointer - BIONIC_TPIDR_OFFSET)
#define bionic_tls_ptr (*(void**)(tls_static_tls + BIONIC_TLS_PTR_OFFSET))
#define pthread_internal_shadow_ptr \
    (*(void**)(tls_static_tls + BIONIC_THREAD_ID_PTR_OFFSET))

static pthread_key_t bionic_tls_cleanup_key;
static pthread_once_t bionic_tls_key_once = PTHREAD_ONCE_INIT;

static void _bionic_tls_cleanup(void *area)
{
    /* A later pthread-key destructor may enter Android code again. Publish an
     * uninitialized slot before releasing this thread's old arena and shadows. */
    bionic_thread_pointer = NULL;
    tls_mapped_bytes = 0;
    tls_inited = 0;
    tls_module_init_count = 0;
    free(*(void **)((char *)area + BIONIC_TLS_PTR_OFFSET));
    free(*(void **)((char *)area + BIONIC_THREAD_ID_PTR_OFFSET));
    munmap(area, BIONIC_TLS_RESERVE_SIZE);
}

static void _bionic_tls_key_init(void)
{
    int error = pthread_key_create(&bionic_tls_cleanup_key, _bionic_tls_cleanup);
    if (error) {
        fprintf(stderr, "HYBRIS: fatal: TLS cleanup key creation failed (%d)\n", error);
        abort();
    }
}

static void hybris_ensure_static_tls(size_t end)
{
    if (end > BIONIC_TLS_RESERVE_SIZE) {
        fprintf(stderr, "HYBRIS: fatal: bionic static TLS exceeds virtual reserve (%zu bytes)\n", end);
        abort();
    }
    if (end < BIONIC_TCB_SIZE) end = BIONIC_TCB_SIZE;
    if (bionic_thread_pointer && end <= tls_mapped_bytes) return;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) abort();
    if (!bionic_thread_pointer) {
        void *area = mmap(NULL, BIONIC_TLS_RESERVE_SIZE, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area == MAP_FAILED) {
            fprintf(stderr, "HYBRIS: fatal: cannot reserve bionic TLS arena: %s\n", strerror(errno));
            abort();
        }
        /* Commit the TCB before publishing its pointer to MRS thunks. */
        if (mprotect(area, page_size, PROT_READ | PROT_WRITE) != 0) {
            int error = errno;
            munmap(area, BIONIC_TLS_RESERVE_SIZE);
            fprintf(stderr, "HYBRIS: fatal: cannot commit bionic TCB: %s\n", strerror(error));
            abort();
        }
        pthread_once(&bionic_tls_key_once, _bionic_tls_key_init);
        int error = pthread_setspecific(bionic_tls_cleanup_key, area);
        if (error) {
            munmap(area, BIONIC_TLS_RESERVE_SIZE);
            fprintf(stderr, "HYBRIS: fatal: TLS cleanup registration failed (%d)\n", error);
            abort();
        }
        tls_mapped_bytes = page_size;
        bionic_thread_pointer = (char *)area + BIONIC_TPIDR_OFFSET;
    }
    if (end > tls_mapped_bytes) {
        size_t mapped = ((end + page_size - 1) / page_size) * page_size;
        if (mapped > BIONIC_TLS_RESERVE_SIZE || mprotect(tls_static_tls + tls_mapped_bytes,
                mapped - tls_mapped_bytes, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "HYBRIS: fatal: cannot commit bionic TLS arena: %s\n", strerror(errno));
            abort();
        }
        tls_mapped_bytes = mapped;
    }
}

__attribute__((visibility("hidden")))
void *hybris_bionic_tls_pointer_slot(void)
{
    return &bionic_thread_pointer;
}

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
                           * arena bytes (zero-filled anonymous pages -- safe
                           * because reserve_solib_segment
                           * is monotonic and never overlaps with prior memsz) */
};

static struct {
    struct hybris_promoted_tls* entries;
    int count;
    int capacity;
    pthread_mutex_t mutex;
} g_promoted_tls = { NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER };

/* Published only after the registry entry and the promoter's TLS are ready.
 * Readers with no missing entries need neither the mutex nor the registry. */
static int g_promoted_tls_published;

static void hybris_apply_static_tls_locked(const struct hybris_promoted_tls* e)
{
    /* Check all of memsz, including the zero-filled .tbss tail. */
    if (e->static_offset > BIONIC_TLS_RESERVE_SIZE ||
        e->segment_size > BIONIC_TLS_RESERVE_SIZE - e->static_offset ||
        e->init_size > e->segment_size) {
        fprintf(stderr, "HYBRIS: fatal: bionic static TLS overflow "
                        "(offset=%zu memsz=%zu, reserve=%u).\n",
                e->static_offset, e->segment_size, BIONIC_TLS_RESERVE_SIZE);
        abort();
    }
    hybris_ensure_static_tls(e->static_offset + e->segment_size);
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
    if (static_offset > BIONIC_TLS_RESERVE_SIZE ||
        segment_size > BIONIC_TLS_RESERVE_SIZE - static_offset ||
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
    __atomic_store_n(&g_promoted_tls_published, g_promoted_tls.count, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_promoted_tls.mutex);
}

__attribute__((__visibility__("default")))
void *_hybris_hook___get_tls_hooks()
{
    TRACE_HOOK("");
    hybris_ensure_static_tls(BIONIC_TCB_SIZE);

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

        tls_inited = 1;
    }

    /* Catch up from any promote that happened on another thread since
     * we last ran. Done outside the !tls_inited gate so a thread that's
     * already inited still picks up newly-promoted modules on its next
     * hooked-libc call.
     *
     * The acquire load checks whether this thread needs new entries. Only
     * the slow path reads the registry and .count, under the writer's mutex.
     * tls_module_init_count is private to this thread.
     *
     * Q's static TLSDESC resolver also calls this hook before returning
     * the offset, so pure TLSDESC first touches catch up without a libc
     * call. IE accesses that bypass that resolver still require an
     * initialized thread. */
    if (__builtin_expect(tls_module_init_count !=
            __atomic_load_n(&g_promoted_tls_published, __ATOMIC_ACQUIRE), 0)) {
        pthread_mutex_lock(&g_promoted_tls.mutex);
        for (int i = tls_module_init_count; i < g_promoted_tls.count; i++) {
            hybris_apply_static_tls_locked(&g_promoted_tls.entries[i]);
        }
        tls_module_init_count = g_promoted_tls.count;
        pthread_mutex_unlock(&g_promoted_tls.mutex);
    }

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
