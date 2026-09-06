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
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <hybris/common/hooks.h>
#include <hybris/properties/properties.h>
#include <android-config.h>
#include "tls_patcher.h"
#include "linker_bridge.h"
#include "logging.h"
#ifdef WANT_ARM_TRACING
#include "wrappers.h"
#endif

/* Preserve the existing Android ABI for namespace boolean results. */
#define bool int
#define LOGD(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

extern int my_property_set(const char *key, const char *value);
extern int my_property_get(const char *key, char *value, const char *default_value);
extern int my_property_list(void (*propfn)(const char *, const char *, void *), void *cookie);

#ifdef WANT_ARM_TRACING
static void (*_android_linker_init)(int sdk_version, void* (*get_hooked_symbol)(const char*, const char*), int enable_linker_gdb_support, hybris_tls_patcher_funcs_t* tls_patcher_funcs, void *(_create_wrapper)(const char*, void*, int), int wrapping_enabled) = NULL;
#else
static void (*_android_linker_init)(int sdk_version, void* (*get_hooked_symbol)(const char*, const char*), int enable_linker_gdb_support, hybris_tls_patcher_funcs_t* tls_patcher_funcs) = NULL;
#endif
void *(*_android_dlopen)(const char* filename, int flag) = NULL;
char *(*_android_dlerror)() = NULL;
void *(*_android_dlsym)(void* handle, const char* symbol) = NULL;
void *(*_android_dlvsym)(void* handle, const char* symbol, const char* version) = NULL;
int (*_android_dladdr)(const void* addr, void* info) = NULL;
int (*_android_dlclose)(void* handle) = NULL;
void *(*_android_dl_unwind_find_exidx)(void *pc, int* pcount) = NULL;
int (*_android_dl_iterate_phdr)(int (*cb)(void* info, size_t size, void* data), void* data) = NULL;
void (*_android_get_LD_LIBRARY_PATH)(char* buffer, size_t buffer_size) = NULL;
void (*_android_update_LD_LIBRARY_PATH)(const char* ld_library_path) = NULL;
void *(*_android_dlopen_ext)(const char* filename, int flag, const void* extinfo) = NULL;
void (*_android_set_application_target_sdk_version)(uint32_t target) = NULL;
int (*_android_get_application_target_sdk_version)() = NULL;
void *(*_android_create_namespace)(const char* name,
                                 const char* ld_library_path,
                                 const char* default_library_path,
                                 uint64_t type,
                                 const char* permitted_when_isolated_path,
                                 void* parent) = NULL;
bool (*_android_init_anonymous_namespace)(const char* shared_libs_sonames,
                                      const char* library_search_path) = NULL;
void (*_android_dlwarning)(void* obj, void (*f)(void*, const char*)) = NULL;
void *(*_android_get_exported_namespace)(const char* name) = NULL;

#if WANT_LINKER_Q
void * (*_android_shared_globals)() = NULL;
#endif

#define LINKER_NAME_JB "jb"
#define LINKER_NAME_MM "mm"
#define LINKER_NAME_N "n"
#define LINKER_NAME_O "o"
#define LINKER_NAME_Q "q"

#if defined(WANT_LINKER_Q)
#define LINKER_VERSION_DEFAULT 29
#define LINKER_NAME_DEFAULT LINKER_NAME_Q
#elif defined(WANT_LINKER_O)
#define LINKER_VERSION_DEFAULT 27
#define LINKER_NAME_DEFAULT LINKER_NAME_O
#elif defined(WANT_LINKER_N)
#define LINKER_VERSION_DEFAULT 25
#define LINKER_NAME_DEFAULT LINKER_NAME_N
#elif defined(WANT_LINKER_MM)
#define LINKER_VERSION_DEFAULT 23
#define LINKER_NAME_DEFAULT LINKER_NAME_MM
#elif defined(WANT_LINKER_JB)
#define LINKER_VERSION_DEFAULT 18
#define LINKER_NAME_DEFAULT LINKER_NAME_JB
#endif

// create string version of default linker for get_android_sdk_version
#define QUOTE(x) #x
#define STRINGIFY(x) QUOTE(x)
#define LINKER_VERSION_DEFAULT_STRING STRINGIFY(LINKER_VERSION_DEFAULT)

int hybris_get_android_sdk_version(void)
{
    static int sdk_version = -1;

    if (sdk_version > 0)
        return sdk_version;

    // in case android-init is patched we can use my_property_get. in case it
    // is not use the default linker. this is such that we don't need the
    // properties patch in android >=8, because properties are read via bionic
    // libc.so starting from android 8, since it is much easier to use the
    // bionic implementation and avoid having to implement all the fancy bionic
    // property features which are mandatory now and cannot be stubbed as
    // previously.
    char value[PROP_VALUE_MAX];
    my_property_get("ro.build.version.sdk", value, LINKER_VERSION_DEFAULT_STRING);

    sdk_version = LINKER_VERSION_DEFAULT;
    if (strlen(value) > 0) {
        sdk_version = atoi(value);
    }

#ifdef UBUNTU_LINKER_OVERRIDES
    /* We override both frieza and turbo here until they are ready to be
     * upgraded to the newer linker. */
    char device_name[PROP_VALUE_MAX];
    memset(device_name, 0, sizeof(device_name));
    my_property_get("ro.build.product", device_name, "");
    if (strlen(device_name) > 0) {
        /* Force SDK version for both frieza/cooler and turbo for the time being */
        if (strcmp(device_name, "frieza") == 0 ||
            strcmp(device_name, "cooler") == 0 ||
            strcmp(device_name, "turbo") == 0)
            sdk_version = 19;
    }
#endif

    char *version_override = getenv("HYBRIS_ANDROID_SDK_VERSION");
    if (version_override)
        sdk_version = atoi(version_override);

    LOGD("Using SDK API version %i\n", sdk_version);

    return sdk_version;
}

static void *linker_handle = NULL;

static void* __hybris_load_linker(const char *path)
{
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "ERROR: Failed to load hybris linker for Android SDK version %d: %s\n",
                hybris_get_android_sdk_version(), dlerror());
        return NULL;
    }
    return handle;
}

static pthread_once_t linker_once = PTHREAD_ONCE_INIT;

/* First-init ownership:
 *   - glibc holds libhybris-common (DF_1_NODELETE) for the process lifetime.
 *   - common holds the Android linker plugin via linker_handle and never
 *     dlcloses it.
 *   - android_dlopen callers hold vendor objects; this path does not
 *     android_dlclose them when a frontend is closed.
 * Public android_* wrappers wait for pthread_once to publish initialization.
 * The bundled linker init paths configure linker state without calling those
 * wrappers. Loader hooks use resolved _android_* entries, avoiding recursive
 * entry into this once control. Any new initialization callback must preserve
 * this constraint; pthread_once does not support recursive initialization. */
static void __hybris_linker_init(void)
{
    LOGD("Linker initialization");
    
    int enable_linker_gdb_support = 0;
    const char *env = getenv("HYBRIS_ENABLE_LINKER_DEBUG_MAP");
    if (env != NULL) {
        if (strcmp(env, "1") == 0) {
            enable_linker_gdb_support = 1;
        }
    }

    int sdk_version = hybris_get_android_sdk_version();

    char path[PATH_MAX];
    const char *name = LINKER_NAME_DEFAULT;

    /* See https://source.android.com/source/build-numbers.html for
     * an overview over available SDK version numbers and which
     * Android version they relate to. */
#if defined(WANT_LINKER_Q)
    if (sdk_version <= 29)
        name = LINKER_NAME_Q;
#endif
#if defined(WANT_LINKER_O)
    if (sdk_version <= 27)
        name = LINKER_NAME_O;
#endif
#if defined(WANT_LINKER_N)
    if (sdk_version <= 25)
        name = LINKER_NAME_N;
#endif
#if defined(WANT_LINKER_MM)
    if (sdk_version <= 23)
        name = LINKER_NAME_MM;
#endif
#if defined(WANT_LINKER_JB)
    if (sdk_version < 21)
        name = LINKER_NAME_JB;
#endif

    const char *linker_dir = LINKER_PLUGIN_DIR;
    /* getauxval to make sure users cannot load custom libraries into
     * setuid processes */
    const char *user_linker_dir = getauxval(AT_SECURE) ?
	    NULL :
	    getenv("HYBRIS_LINKER_DIR");
    if (user_linker_dir)
        linker_dir = user_linker_dir;

    snprintf(path, PATH_MAX, "%s/%s.so", linker_dir, name);

    LOGD("Loading linker from %s..", path);

    linker_handle = __hybris_load_linker(path);
    if (!linker_handle)
        exit(1);

    /* Load all necessary symbols we need from the linker */
    _android_linker_init = dlsym(linker_handle, "android_linker_init");
    _android_dlopen = dlsym(linker_handle, "android_dlopen");
    _android_dlerror = dlsym(linker_handle, "android_dlerror");
    _android_dlsym = dlsym(linker_handle, "android_dlsym");
    _android_dlvsym = dlsym(linker_handle, "android_dlvsym");
    _android_dladdr = dlsym(linker_handle, "android_dladdr");
    _android_dlclose = dlsym(linker_handle, "android_dlclose");
    _android_dl_unwind_find_exidx = dlsym(linker_handle, "android_dl_unwind_find_exidx");
    _android_dl_iterate_phdr = dlsym(linker_handle, "android_dl_iterate_phdr");
    _android_get_LD_LIBRARY_PATH = dlsym(linker_handle, "android_get_LD_LIBRARY_PATH");
    _android_update_LD_LIBRARY_PATH = dlsym(linker_handle, "android_update_LD_LIBRARY_PATH");
    _android_dlopen_ext = dlsym(linker_handle, "android_dlopen_ext");
    _android_set_application_target_sdk_version = dlsym(linker_handle, "android_set_application_target_sdk_version");
    _android_get_application_target_sdk_version = dlsym(linker_handle, "android_get_application_target_sdk_version");
    _android_create_namespace = dlsym(linker_handle, "android_create_namespace");
    _android_init_anonymous_namespace = dlsym(linker_handle, "android_init_anonymous_namespace");
    _android_dlwarning = dlsym(linker_handle, "android_dlwarning");
    _android_get_exported_namespace = dlsym(linker_handle, "android_get_exported_namespace");
#if WANT_LINKER_Q
    _android_shared_globals = dlsym(linker_handle, "android_shared_globals");
#endif

    /* Create TLS patcher function pointers struct */
    hybris_tls_patcher_funcs_t tls_patcher_funcs = {0};
#ifdef __aarch64__
    tls_patcher_funcs.patch_tls = hybris_patch_tls;
    tls_patcher_funcs.register_thunk_region = hybris_register_thunk_region;
    tls_patcher_funcs.count_tls = hybris_count_tls;
#endif
    /* Wire on every arch: linker_tls.cpp uses this whenever it promotes
     * a TLS module to a static-TLS slot (IE or TLSDESC reloc), which is
     * arch-independent. Without this callback the bionic .so's __thread
     * variables would read zero-init'd memory in tls_static_tls instead
     * of their declared initial values from .tdata. */
    tls_patcher_funcs.init_static_tls_for_thread = _hybris_init_static_tls_for_thread;

    /* Now its time to setup the linker itself */
#ifdef WANT_ARM_TRACING
    _android_linker_init(sdk_version, hybris_get_hooked_symbol, enable_linker_gdb_support, &tls_patcher_funcs, create_wrapper, wrappers_enabled());
#else
    _android_linker_init(sdk_version, hybris_get_hooked_symbol, enable_linker_gdb_support, &tls_patcher_funcs);
#endif

    if (_android_set_application_target_sdk_version) {
        _android_set_application_target_sdk_version(sdk_version);
    }
}

#define ENSURE_LINKER_IS_LOADED() \
    do { \
        if (pthread_once(&linker_once, __hybris_linker_init) != 0) \
            abort(); \
    } while (0)

/* NOTE: As we're not linking directly with the linker anymore
 * but several users are using android_* functions directly we
 * have to export them here. */

void* android_dlopen(const char* filename, int flag)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlopen) {
        return NULL;
    }

    return _android_dlopen(filename, flag);
}

char* android_dlerror()
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlerror) {
        return NULL;
    }

    return _android_dlerror();
}

void* android_dlsym(void* handle, const char* symbol)
{
    ENSURE_LINKER_IS_LOADED();

    // do not use hybris properties for older linkers
    if (hybris_get_android_sdk_version() < 27) {
        if (!strcmp(symbol, "property_list")) {
            return my_property_list;
        }
        if (!strcmp(symbol, "property_get")) {
            return my_property_get;
        }
        if (!strcmp(symbol, "property_set")) {
            return my_property_set;
        }
    }

    if (!_android_dlsym) {
        return NULL;
    }

    return _android_dlsym(handle, symbol);
}

void* android_dlvsym(void* handle, const char* symbol, const char* version)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlvsym) {
        return NULL;
    }

    return _android_dlvsym(handle, symbol, version);
}

int android_dladdr(const void* addr, void* info)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dladdr) {
        return 0;
    }

    return _android_dladdr(addr, info);
}

int android_dlclose(void* handle)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlclose) {
        return 0;
    }

    return _android_dlclose(handle);
}

void *android_dl_unwind_find_exidx(void *pc, int* pcount)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dl_unwind_find_exidx) {
        return NULL;
    }

    return _android_dl_unwind_find_exidx(pc, pcount);
}

int android_dl_iterate_phdr(int (*cb)(void* info, size_t size, void* data), void* data)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dl_iterate_phdr) {
        return 0;
    }

    return _android_dl_iterate_phdr(cb, data);
}

void android_get_LD_LIBRARY_PATH(char* buffer, size_t buffer_size)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_get_LD_LIBRARY_PATH) {
        return;
    }

    _android_get_LD_LIBRARY_PATH(buffer, buffer_size);
}

void android_update_LD_LIBRARY_PATH(const char* ld_library_path)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_update_LD_LIBRARY_PATH) {
        return;
    }

    _android_update_LD_LIBRARY_PATH(ld_library_path);
}

void* android_dlopen_ext(const char* filename, int flag, const void* extinfo)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlopen_ext) {
        return NULL;
    }

    return _android_dlopen_ext(filename, flag, extinfo);
}

void android_set_application_target_sdk_version(uint32_t target)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_set_application_target_sdk_version) {
        return;
    }

    _android_set_application_target_sdk_version(target);
}

int android_get_application_target_sdk_version()
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_get_application_target_sdk_version) {
        return 0;
    }

    return _android_get_application_target_sdk_version();
}

struct android_namespace_t* android_create_namespace(const char* name,
                                                     const char* ld_library_path,
                                                     const char* default_library_path,
                                                     uint64_t type,
                                                     const char* permitted_when_isolated_path,
                                                     struct android_namespace_t* parent)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_create_namespace) {
        return NULL;
    }

    return _android_create_namespace(name, ld_library_path, default_library_path, type, permitted_when_isolated_path, parent);
}

bool android_init_anonymous_namespace(const char* shared_libs_sonames,
                                      const char* library_search_path)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_init_anonymous_namespace) {
        return 0;
    }

    return _android_init_anonymous_namespace(shared_libs_sonames, library_search_path);
}

void android_dlwarning(void* obj, void (*f)(void*, const char*))
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_dlwarning) {
        return;
    }

    _android_dlwarning(obj, f);
}

struct android_namespace_t* android_get_exported_namespace(const char* name)
{
    ENSURE_LINKER_IS_LOADED();

    if (!_android_get_exported_namespace) {
        return NULL;
    }

    return _android_get_exported_namespace(name);
}

void* hybris_dlopen(const char* filename, int flag)
{
    return android_dlopen(filename, flag);
}

char* hybris_dlerror()
{
    return android_dlerror();
}

void* hybris_dlsym(void* handle, const char* symbol)
{
    return android_dlsym(handle, symbol);
}

void* hybris_dlvsym(void* handle, const char* symbol, const char* version)
{
    return android_dlvsym(handle, symbol, version);
}

int hybris_dladdr(const void* addr, void* info)
{
    return android_dladdr(addr, info);
}

int hybris_dlclose(void* handle)
{
    return android_dlclose(handle);
}

void *hybris_dl_unwind_find_exidx(void *pc, int* pcount)
{
    return android_dl_unwind_find_exidx(pc, pcount);
}

int hybris_dl_iterate_phdr(int (*cb)(void* info, size_t size, void* data), void* data)
{
    return android_dl_iterate_phdr(cb, data);
}

void hybris_get_LD_LIBRARY_PATH(char* buffer, size_t buffer_size)
{
    android_get_LD_LIBRARY_PATH(buffer, buffer_size);
}

void hybris_update_LD_LIBRARY_PATH(const char* ld_library_path)
{
    android_update_LD_LIBRARY_PATH(ld_library_path);
}

void* hybris_dlopen_ext(const char* filename, int flag, const void* extinfo)
{
    return android_dlopen_ext(filename, flag, extinfo);
}

void hybris_set_application_target_sdk_version(uint32_t target)
{
    android_set_application_target_sdk_version(target);
}

int hybris_get_application_target_sdk_version()
{
    return android_get_application_target_sdk_version();
}

void* hybris_create_namespace(const char* name,
                                                     const char* ld_library_path,
                                                     const char* default_library_path,
                                                     uint64_t type,
                                                     const char* permitted_when_isolated_path,
                                                     void* parent)
{
    return android_create_namespace(name, ld_library_path, default_library_path, type, permitted_when_isolated_path, parent);
}

bool hybris_init_anonymous_namespace(const char* shared_libs_sonames,
                                      const char* library_search_path)
{
    return android_init_anonymous_namespace(shared_libs_sonames, library_search_path);
}

void hybris_dlwarning(void* obj, void (*f)(void*, const char*))
{
    android_dlwarning(obj, f);
}

void* hybris_get_exported_namespace(const char* name)
{
    return android_get_exported_namespace(name);
}

