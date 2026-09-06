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

#ifndef HYBRIS_LINKER_BRIDGE_H
#define HYBRIS_LINKER_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

/* Internal common-module boundary. Public android_* and hybris_* declarations
 * remain in the installed headers. Keep these helpers out of the DSO ABI. */
__attribute__((visibility("hidden")))
int hybris_get_android_sdk_version(void);
__attribute__((visibility("hidden")))
void *hybris_get_hooked_symbol(const char *symbol, const char *requester);

/* Existing exported TLS callback and backend pointers retain their ABI.
 * Loader hooks call these resolved entries without reentering public wrappers. */
void _hybris_init_static_tls_for_thread(size_t static_offset,
                                      const void *init_ptr,
                                      size_t init_size, size_t segment_size);
extern void *(*_android_dlopen)(const char* filename, int flag);
extern char *(*_android_dlerror)();
extern void *(*_android_dlsym)(void* handle, const char* symbol);
extern void *(*_android_dlvsym)(void* handle, const char* symbol, const char* version);
extern int (*_android_dladdr)(const void* addr, void* info);
extern int (*_android_dlclose)(void* handle);
extern void *(*_android_dl_unwind_find_exidx)(void *pc, int* pcount);
extern int (*_android_dl_iterate_phdr)(int (*cb)(void* info, size_t size, void* data), void* data);
extern void (*_android_get_LD_LIBRARY_PATH)(char* buffer, size_t buffer_size);
extern void (*_android_update_LD_LIBRARY_PATH)(const char* ld_library_path);
extern void *(*_android_dlopen_ext)(const char* filename, int flag, const void* extinfo);
extern void (*_android_set_application_target_sdk_version)(uint32_t target);
extern int (*_android_get_application_target_sdk_version)();
extern void *(*_android_create_namespace)(const char* name,
                                 const char* ld_library_path,
                                 const char* default_library_path,
                                 uint64_t type,
                                 const char* permitted_when_isolated_path,
                                 void* parent);
extern int (*_android_init_anonymous_namespace)(const char* shared_libs_sonames,
                                      const char* library_search_path);
extern void (*_android_dlwarning)(void* obj, void (*f)(void*, const char*));
extern void *(*_android_get_exported_namespace)(const char* name);

#if WANT_LINKER_Q
extern void * (*_android_shared_globals)();
#endif

#endif
