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

#ifndef TLS_PATCHER_H
#define TLS_PATCHER_H

#include <stddef.h>

/* Shared by the patcher and Q linker reservation; includes 16-byte alignment. */
#define HYBRIS_TLS_THUNK_SIZE 80

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hybris_tls_patcher_t)(void* segment_addr, size_t segment_size, const char* library_name);
typedef void (*hybris_register_thunk_region_t)(void* start, size_t size);
typedef size_t (*hybris_count_tls_t)(void* segment_addr, size_t segment_size);
/* Record a promoted TLS module and copy its .tdata into the calling
 * thread's bionic static-TLS area. The linker calls this from
 * promote_tls_module_to_static() so the registering thread sees the
 * correct initial values for `__thread` variables in the dlopened
 * bionic .so. Hooks owns the .tdata copy thereafter; other threads
 * replay the registry on first hooked-libc call. `segment_size` is
 * memsz: filesz (`init_size`) bytes get memcpy'd, the trailing
 * `segment_size - init_size` bytes are .tbss zeros that the static-TLS
 * area already provides — but the bounds check covers all of memsz so a
 * future segment that overruns BIONIC_STATIC_TLS_SIZE aborts loudly. */
typedef void (*hybris_init_static_tls_for_thread_t)(size_t static_offset,
                                                    const void* init_ptr,
                                                    size_t init_size,
                                                    size_t segment_size);

/* Function pointers passed from hooks.c to the Android linker */
struct hybris_tls_patcher_funcs {
    hybris_tls_patcher_t patch_tls;
    hybris_register_thunk_region_t register_thunk_region;
    hybris_count_tls_t count_tls;
    hybris_init_static_tls_for_thread_t init_static_tls_for_thread;
};
typedef struct hybris_tls_patcher_funcs hybris_tls_patcher_funcs_t;

/* Register a thunk region for the current library being patched */
void hybris_register_thunk_region(void* start, size_t size);

/* Count MRS TPIDR_EL0 instructions in a code segment */
size_t hybris_count_tls(void* segment_addr, size_t segment_size);

/* Patch TLS accesses in the given segment at runtime */
void hybris_patch_tls(void* segment_addr, size_t segment_size, const char* library_name);

#ifdef __cplusplus
}
#endif

#endif /* TLS_PATCHER_H */
