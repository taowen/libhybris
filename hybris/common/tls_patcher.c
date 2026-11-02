/*
 * Copyright (c) 2025 Nikita Ukhrenkov <thekit@disroot.org>
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

#include "tls_patcher.h"
#include "bionic_tls.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libgen.h>
#include <pthread.h>
#include <unistd.h>


/* Forward declarations for arch-specific functions (#included at bottom) */
static void hybris_patch_tls_arch(void* segment_addr, size_t segment_size, int tls_offset);
static size_t hybris_count_tls_arch(void* segment_addr, size_t segment_size);

/* Current thunk region. Only one is active at a time -- the linker registers
 * a region for each library immediately before patching it. */
static struct {
    void* start;
    size_t size;
    size_t used;
} current_thunk_region;
static pthread_mutex_t thunk_mutex = PTHREAD_MUTEX_INITIALIZER;

void hybris_register_thunk_region(void* start, size_t size) {
    pthread_mutex_lock(&thunk_mutex);
    current_thunk_region.start = start;
    current_thunk_region.size = size;
    current_thunk_region.used = 0;
    pthread_mutex_unlock(&thunk_mutex);
}

/* Allocate a thunk from the current region. Aborts on failure -- the region
 * is exactly sized by the counting pass, so running out is a bug. */
static void* hybris_allocate_thunk_near(void* target_addr, size_t thunk_size) {
    pthread_mutex_lock(&thunk_mutex);

    size_t aligned_size = (thunk_size + 15) & ~(size_t)15;

    if (!current_thunk_region.start ||
        current_thunk_region.used + aligned_size > current_thunk_region.size) {
        fprintf(stderr, "HYBRIS: fatal: thunk region exhausted (used %zu / %zu, need %zu) for target %p\n",
                current_thunk_region.used, current_thunk_region.size, aligned_size, target_addr);
        abort();
    }

    void* result = (char*)current_thunk_region.start + current_thunk_region.used;

    /* Verify within 128MB branch range */
    uintptr_t target = (uintptr_t)target_addr;
    uintptr_t thunk = (uintptr_t)result;
    uintptr_t distance = (target > thunk) ? (target - thunk) : (thunk - target);
    if (distance > 128 * 1024 * 1024) {
        fprintf(stderr, "HYBRIS: fatal: thunk at %p is %zuMB from target %p (max 128MB)\n",
                result, distance / (1024 * 1024), target_addr);
        abort();
    }

    current_thunk_region.used += aligned_size;
    pthread_mutex_unlock(&thunk_mutex);
    return result;
}

/* Calculate the fixed glibc TP offset of the arena-pointer slot. */
static int hybris_calculate_tls_offset(void) {
    void* tp = __builtin_thread_pointer();
    void* tls_area = hybris_bionic_tls_pointer_slot();
    return (int)((uintptr_t)tls_area - (uintptr_t)tp) / sizeof(uintptr_t);
}

size_t hybris_count_tls(void* segment_addr, size_t segment_size) {
    return hybris_count_tls_arch(segment_addr, segment_size);
}

void hybris_patch_tls(void* segment_addr, size_t segment_size, const char* library_name) {
    HYBRIS_DEBUG_LOG(HOOKS, "Patching TLS accesses in %s", library_name);

    int tls_offset = hybris_calculate_tls_offset();
    HYBRIS_DEBUG_LOG(HOOKS, "Offset from thread pointer to hybris TLS pointer slot (in words): %d",
        tls_offset);

    hybris_patch_tls_arch(segment_addr, segment_size, tls_offset);
}

#ifdef __aarch64__
#include "tls_patcher_aarch64.c"
#endif
