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

#include <hybris/common/binding.h>

#include "hooks_shm.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdio_ext.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <inttypes.h>
#include <strings.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/xattr.h>
#include <grp.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <stdarg.h>
#include <wchar.h>
#include <sched.h>
#include <pwd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/signalfd.h>
#include <sys/uio.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include <netdb.h>
#include <unistd.h>
#include <syslog.h>
#include <locale.h>
#include <sys/syscall.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/uio.h>

#include <sys/mman.h>
#include <libgen.h>
#include <mntent.h>

#include <hybris/properties/properties.h>

// using private implementations
extern int my_property_set(const char *key, const char *value);
extern int my_property_get(const char *key, char *value, const char *default_value);
extern int my_property_list(void (*propfn)(const char *key, const char *value, void *cookie), void *cookie);

#include <hybris/common/hooks.h>

#include <android-config.h>

// this is also used in bionic:
#define bool int

#include "dso_handle_counters.h"
#include "tls_patcher.h"

#ifdef WANT_ARM_TRACING
#include "wrappers.h"
#endif

static locale_t hybris_locale;
static int locale_inited = 0;
static hybris_hook_cb hook_callback = NULL;

#include "bionic_tls.h"
#include "bionic_sync.h"
#include "bionic_stdio.h"

#include "linker_bridge.h"

/* TODO:
*  - Check if the int arguments at attr_set/get match the ones at Android
*  - Check how to deal with memory leaks (specially with static initializers)
*  - Check for shared rwlock
*/

#define ANDROID_RWLOCKATTR_SHARED_MASK 0x0010


#define MALI_HIST_DUMP_THREAD_NAME "mali-hist-dump"

/* Debug */
#include "logging.h"
#define LOGD(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

#define TRACE_HOOK(message, ...) \
        HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__);

/*
 * symbols that can be hooked directly shall use HOOK_DIRECT
 * - during debug they will be redirected to a function that traces the calls
 * - during normal execution they will be redirected to the glibc equivalent
 */
#define HOOK_DIRECT(symbol) {#symbol, symbol, _hybris_hook_##symbol}

/*
 * same as above but for symbols who have no dedicated debug wrapper
 */
#define HOOK_DIRECT_NO_DEBUG(symbol) {#symbol, symbol, symbol}

/*
 * symbols that can only be hooked directly to a (symbol with a different name)
 * shall use HOOK_TO
 */
#define HOOK_TO(symbol, hook) {#symbol, hook, hook}

/*
 * symbols that can only be hooked indirectly shall use HOOK
 */
#define HOOK_INDIRECT(symbol) {#symbol, _hybris_hook_##symbol, _hybris_hook_##symbol}

/* we have a value p:
 *  - if p <= ANDROID_TOP_ADDR_VALUE_MUTEX then it is an android mutex, not one we processed
 *  - if p > VMALLOC_END, then the pointer is not a result of malloc ==> it is an shm offset
 */

struct _hook {
    const char *name;
    void *func;
    void *debug_func;
};



/*
 * utils, such as malloc, memcpy
 *
 * Useful to handle hacks such as the one applied for Nvidia, and to
 * avoid crashes. Also we need to hook all memory allocation related
 * ones to make sure all are using the same allocator implementation.
 *
 * */

static void *_hybris_hook_malloc(size_t size)
{
    TRACE_HOOK("size %zu", size);

    void *res = malloc(size);

    TRACE_HOOK("res %p", res);

    return res;
}

static void *_hybris_hook_aligned_alloc(size_t alignment, size_t size)
{
    TRACE_HOOK("alignment %zu size %zu", alignment, size);

    void *res = aligned_alloc(alignment, size);

    TRACE_HOOK("res %p", res);

    return res;
}

#ifdef WANT_ADRENO_QUIRKS
static void *_hybris_hook_malloc45(size_t size)
{
    TRACE_HOOK("size %zu", size);

    if (size == 4) size = 5;

    void *res = malloc(size);

    TRACE_HOOK("res %p", res);

    return res;
}
#endif

static size_t _hybris_hook_malloc_usable_size (void *ptr)
{
    TRACE_HOOK("ptr %p", ptr);

    return malloc_usable_size(ptr);
}

static void *_hybris_hook_memcpy(void *dst, const void *src, size_t len)
{
    TRACE_HOOK("dst %p src %p len %zu", dst, src, len);

    if (src == dst) {
        return dst;
    }

    if (src == NULL || dst == NULL)
        return dst;

    return memcpy(dst, src, len);
}

static int _hybris_hook_memcmp(const void *s1, const void *s2, size_t n)
{
    TRACE_HOOK("s1 %p '%s' s2 %p '%s' n %zu", s1, (char*) s1, s2, (char*) s2, n);

    return memcmp(s1, s2, n);
}

static size_t _hybris_hook_strlen(const char *s)
{
    TRACE_HOOK("s '%s'", s);

    if (s == NULL)
        return -1;

    return strlen(s);
}

static pid_t _hybris_hook_gettid(void)
{
    TRACE_HOOK("");

    return syscall(__NR_gettid);
}

/*
 * Main pthread functions
 *
 * Custom implementations to workaround difference between Bionic and Glibc.
 * Our own pthread_create helps avoiding direct handling of TLS.
 *
 * */

/* Wrapper for bionic threads: ensure bionic_tls is allocated before
 * the bionic start_routine runs. */
struct _hybris_thread_wrapper_args {
    void *(*real_start)(void*);
    void *real_arg;
};

static void *_hybris_thread_wrapper(void *wrapper_arg) {
    struct _hybris_thread_wrapper_args args =
        *(struct _hybris_thread_wrapper_args *)wrapper_arg;
    free(wrapper_arg);

    /* Force bionic_tls allocation for this thread */
    _hybris_hook___get_tls_hooks();

    return args.real_start(args.real_arg);
}

static int _hybris_hook_pthread_create(pthread_t *thread, const pthread_attr_t *__attr,
                             void *(*start_routine)(void*), void *arg)
{
    pthread_attr_t *realattr = NULL;

    TRACE_HOOK("thread %p attr %p", thread, __attr);

    if (__attr != NULL)
        realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    /* Wrap start_routine to initialize bionic_tls on the new thread */
    struct _hybris_thread_wrapper_args *wrapper_args =
        malloc(sizeof(struct _hybris_thread_wrapper_args));
    if (!wrapper_args) {
        fprintf(stderr, "HYBRIS: fatal: failed to allocate thread wrapper args\n");
        abort();
    }
    wrapper_args->real_start = start_routine;
    wrapper_args->real_arg = arg;
    return pthread_create(thread, realattr, _hybris_thread_wrapper, wrapper_args);
}

static int _hybris_hook_pthread_kill(pthread_t thread, int sig)
{
    TRACE_HOOK("thread %llu sig %d", (unsigned long long) thread, sig);

    if (thread == 0)
        return ESRCH;

    return pthread_kill(thread, sig);
}

static int _hybris_hook_pthread_setspecific(pthread_key_t key, const void *ptr)
{
    TRACE_HOOK("key %d ptr %" PRIdPTR, key, (intptr_t) ptr);

    return pthread_setspecific(key, ptr);
}

static void* _hybris_hook_pthread_getspecific(pthread_key_t key)
{
    TRACE_HOOK("key %d", key);

    // see android_bionic/tests/pthread_test.cpp, test static_pthread_key_used_before_creation
    if(!key) return NULL;

    return pthread_getspecific(key);
}

/*
 * pthread_attr_* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_attr_t struct differences between Bionic and Glibc.
 *
 * */

static int _hybris_hook_pthread_attr_init(pthread_attr_t *__attr)
{
    pthread_attr_t *realattr;

    TRACE_HOOK("attr %p", __attr);

    realattr = malloc(sizeof(pthread_attr_t));
    *((uintptr_t *)__attr) = (uintptr_t) realattr;

    return pthread_attr_init(realattr);
}

static int _hybris_hook_pthread_attr_destroy(pthread_attr_t *__attr)
{
    int ret;
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p", __attr);

    ret = pthread_attr_destroy(realattr);
    /* We need to release the memory allocated at _hybris_hook_pthread_attr_init
     * Possible side effects if destroy is called without our init */
    free(realattr);

    return ret;
}

static int _hybris_hook_pthread_attr_setdetachstate(pthread_attr_t *__attr, int state)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p state %d", __attr, state);

    return pthread_attr_setdetachstate(realattr, state);
}

static int _hybris_hook_pthread_attr_getdetachstate(pthread_attr_t const *__attr, int *state)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p state %p", __attr, state);

    return pthread_attr_getdetachstate(realattr, state);
}

static int _hybris_hook_pthread_attr_setschedpolicy(pthread_attr_t *__attr, int policy)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p policy %d", __attr, policy);

    return pthread_attr_setschedpolicy(realattr, policy);
}

static int _hybris_hook_pthread_attr_getschedpolicy(pthread_attr_t const *__attr, int *policy)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p policy %p", __attr, policy);

    return pthread_attr_getschedpolicy(realattr, policy);
}

static int _hybris_hook_pthread_attr_setschedparam(pthread_attr_t *__attr, struct sched_param const *param)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p param %p", __attr, param);

    return pthread_attr_setschedparam(realattr, param);
}

static int _hybris_hook_pthread_attr_getschedparam(pthread_attr_t const *__attr, struct sched_param *param)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p param %p", __attr, param);

    return pthread_attr_getschedparam(realattr, param);
}

static int _hybris_hook_pthread_attr_setstacksize(pthread_attr_t *__attr, size_t stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack size %zu", __attr, stack_size);

    return pthread_attr_setstacksize(realattr, stack_size);
}

static int _hybris_hook_pthread_attr_getstacksize(pthread_attr_t const *__attr, size_t *stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack size %p", __attr, stack_size);

    return pthread_attr_getstacksize(realattr, stack_size);
}

static int _hybris_hook_pthread_attr_setstackaddr(pthread_attr_t *__attr, void *stack_addr)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack addr %p", __attr, stack_addr);

    return pthread_attr_setstackaddr(realattr, stack_addr);
}

static int _hybris_hook_pthread_attr_getstackaddr(pthread_attr_t const *__attr, void **stack_addr)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack addr %p", __attr, stack_addr);

    return pthread_attr_getstackaddr(realattr, stack_addr);
}

static int _hybris_hook_pthread_attr_setstack(pthread_attr_t *__attr, void *stack_base, size_t stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack base %p stack size %zu", __attr,
               stack_base, stack_size);

    return pthread_attr_setstack(realattr, stack_base, stack_size);
}

static int _hybris_hook_pthread_attr_getstack(pthread_attr_t const *__attr, void **stack_base, size_t *stack_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p stack base %p stack size %p", __attr,
               stack_base, stack_size);

    return pthread_attr_getstack(realattr, stack_base, stack_size);
}

static int _hybris_hook_pthread_attr_setguardsize(pthread_attr_t *__attr, size_t guard_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p guard size %zu", __attr, guard_size);

    return pthread_attr_setguardsize(realattr, guard_size);
}

static int _hybris_hook_pthread_attr_getguardsize(pthread_attr_t const *__attr, size_t *guard_size)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p guard size %p", __attr, guard_size);

    return pthread_attr_getguardsize(realattr, guard_size);
}

static int _hybris_hook_pthread_attr_setscope(pthread_attr_t *__attr, int scope)
{
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p scope %d", __attr, scope);

    return pthread_attr_setscope(realattr, scope);
}

static int _hybris_hook_pthread_attr_getscope(pthread_attr_t const *__attr)
{
    int scope;
    pthread_attr_t *realattr = (pthread_attr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p", __attr);

    /* Android doesn't have the scope attribute because it always
     * returns PTHREAD_SCOPE_SYSTEM */
    pthread_attr_getscope(realattr, &scope);

    return scope;
}

static int _hybris_hook_pthread_getattr_np(pthread_t thid, pthread_attr_t *__attr)
{
    pthread_attr_t *realattr;

    TRACE_HOOK("attr %p", __attr);

    realattr = malloc(sizeof(pthread_attr_t));
    *((uintptr_t *)__attr) = (uintptr_t) realattr;

    return pthread_getattr_np(thid, realattr);
}

int _hybris_hook_pthread_setname_np(pthread_t thread, const char *name)
{
    TRACE_HOOK("thread %llu name %s", (unsigned long long) thread, name);

#ifdef MALI_QUIRKS
    if (strcmp(name, MALI_HIST_DUMP_THREAD_NAME) == 0) {
        HYBRIS_DEBUG_LOG(HOOKS, "%s: Found mali-hist-dump thread, killing it ...",
                         __FUNCTION__);

        if (thread != pthread_self()) {
            HYBRIS_DEBUG_LOG(HOOKS, "%s: -> Failed, as calling thread is not mali-hist-dump itself",
                             __FUNCTION__);
            return 0;
        }

        pthread_exit((void*) thread);

        return 0;
    }
#endif

    return pthread_setname_np(thread, name);
}

/* Bionic implementation of pthread_cleanup_push/pop doesn't support C++ exceptions
   and thread cancelation. We only make sure to call the cleanup routine when
   requested. We duplicate the bionic cleanup struct here for our purposes. */

typedef void (*bionic___pthread_cleanup_func_t)(void*);

typedef struct bionic___pthread_cleanup_t {
    struct bionic___pthread_cleanup_t*  __cleanup_prev;     /* unused */
    bionic___pthread_cleanup_func_t     __cleanup_routine;
    void*                               __cleanup_arg;
} bionic___pthread_cleanup_t;

static void _hybris_hook___pthread_cleanup_push(void *bionic_cleanup, void *routine, void *arg)
{
    bionic___pthread_cleanup_t *cleanup = bionic_cleanup;

    TRACE_HOOK("cleanup %p routine %p arg %p", cleanup, routine, arg);
    cleanup->__cleanup_routine = routine;
    cleanup->__cleanup_arg = arg;
}

static void _hybris_hook___pthread_cleanup_pop(void *bionic_cleanup, int execute)
{
    bionic___pthread_cleanup_t *cleanup = bionic_cleanup;

    TRACE_HOOK("cleanup %p execute %d", cleanup, execute);
    if (execute)
        cleanup->__cleanup_routine(cleanup->__cleanup_arg);
}

#define min(X,Y) (((X) < (Y)) ? (X) : (Y))

static pid_t _hybris_hook_pthread_gettid_np(pthread_t t)
{
    TRACE_HOOK("thread %lu", (unsigned long) t);

    // glibc doesn't offer us a way to retrieve the thread id for a
    // specific thread. However pthread_t is defined as unsigned
    // long int and is the thread id so we can just copy it over
    // into a pid_t.
    pid_t tid;
    memcpy(&tid, &t, min(sizeof(tid), sizeof(t)));
    return tid;
}

static int _hybris_hook___set_errno(int oi_errno)
{
    TRACE_HOOK("errno %d", oi_errno);

    errno = oi_errno;

    return -1;
}

/* "struct dirent" from bionic/libc/include/dirent.h */
struct bionic_dirent {
    uint64_t         d_ino;
    int64_t          d_off;
    unsigned short   d_reclen;
    unsigned char    d_type;
    char             d_name[256];
};

static struct bionic_dirent *_hybris_hook_readdir(DIR *dirp)
{
    /**
     * readdir(3) manpage says:
     *  The data returned by readdir() may be overwritten by subsequent calls
     *  to readdir() for the same directory stream.
     *
     * XXX: At the moment, for us, the data will be overwritten even by
     * subsequent calls to /different/ directory streams. Eventually fix that
     * (e.g. by storing per-DIR * bionic_dirent structs, and removing them on
     * closedir, requires hooking of all funcs returning/taking DIR *) and
     * handling the additional data attachment there)
     **/

    static struct bionic_dirent result;

    TRACE_HOOK("dirp %p", dirp);

    struct dirent *real_result = readdir(dirp);
    if (!real_result) {
        return NULL;
    }

    result.d_ino = real_result->d_ino;
    result.d_off = real_result->d_off;
    result.d_reclen = real_result->d_reclen;
    result.d_type = real_result->d_type;
    memcpy(result.d_name, real_result->d_name, sizeof(result.d_name));

    // Make sure the string is zero-terminated, even if cut off (which
    // shouldn't happen, as both bionic and glibc have d_name defined
    // as fixed array of 256 chars)
    result.d_name[sizeof(result.d_name)-1] = '\0';
    return &result;
}

static int _hybris_hook_readdir_r(DIR *dir, struct bionic_dirent *entry,
        struct bionic_dirent **result)
{
    struct dirent entry_r;
    struct dirent *result_r;

    TRACE_HOOK("dir %p entry %p result %p", dir, entry, result);

    int res = readdir_r(dir, &entry_r, &result_r);

    if (res == 0) {
        if (result_r != NULL) {
            *result = entry;

            entry->d_ino = entry_r.d_ino;
            entry->d_off = entry_r.d_off;
            entry->d_reclen = entry_r.d_reclen;
            entry->d_type = entry_r.d_type;
            memcpy(entry->d_name, entry_r.d_name, sizeof(entry->d_name));

            // Make sure the string is zero-terminated, even if cut off (which
            // shouldn't happen, as both bionic and glibc have d_name defined
            // as fixed array of 256 chars)
            entry->d_name[sizeof(entry->d_name) - 1] = '\0';
        } else {
            *result = NULL;
        }
    }

    return res;
}

static int _hybris_hook_alphasort(struct bionic_dirent **a,
                                  struct bionic_dirent **b)
{
    return strcoll((*a)->d_name, (*b)->d_name);
}

static int _hybris_hook_versionsort(struct bionic_dirent **a,
                          struct bionic_dirent **b)
{
    return strverscmp((*a)->d_name, (*b)->d_name);
}

static int _hybris_hook_scandirat(int fd, const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    struct dirent **namelist_r;
    struct bionic_dirent **result;
    struct bionic_dirent *filter_r;

    int i = 0;
    size_t nItems = 0;

    int res = scandirat(fd, dir, &namelist_r, NULL, NULL);

    if (res != 0 && namelist_r != NULL) {

        result = malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                        free(result[i]);
                    free(result);
                    return -1;
            }
            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) {//apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }
        if (nItems && compar != NULL) // sort
            qsort(result, nItems, sizeof(struct bionic_dirent *), (int (*)(const void *, const void *)) compar);

        *namelist = result;
    }

    return nItems;
}

static int _hybris_hook_scandir(const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    return _hybris_hook_scandirat(AT_FDCWD, dir, namelist, filter, compar);
}

static inline void swap(void **a, void **b)
{
    void *tmp = *a;
    *a = *b;
    *b = tmp;
}

static int _hybris_hook_getaddrinfo(const char *hostname, const char *servname,
    const struct addrinfo *hints, struct addrinfo **res)
{
    struct addrinfo *fixed_hints = NULL;

    TRACE_HOOK("hostname '%s' servname '%s' hints %p res %p",
               hostname, servname, hints, res);

    if (hints) {
        fixed_hints = (struct addrinfo*) malloc(sizeof(struct addrinfo));
        memcpy(fixed_hints, hints, sizeof(struct addrinfo));
        // fix bionic -> glibc missmatch
        swap((void**)&(fixed_hints->ai_canonname), (void**)&(fixed_hints->ai_addr));
    }

    int result = getaddrinfo(hostname, servname, fixed_hints, res);

    if (fixed_hints)
        free(fixed_hints);

    if(result == 0) {
        // fix bionic <- glibc missmatch
        struct addrinfo *it = *res;
        while (it) {
            swap((void**) &(it->ai_canonname), (void**) &(it->ai_addr));
            it = it->ai_next;
        }
    }

    return result;
}

static void _hybris_hook_freeaddrinfo(struct addrinfo *__ai)
{
    TRACE_HOOK("ai %p", __ai);

    if (__ai == NULL)
        return;

    struct addrinfo *it = __ai;
    while (it) {
        swap((void**) &(it->ai_canonname), (void**) &(it->ai_addr));
        it = it->ai_next;
    }

    freeaddrinfo(__ai);
}

extern long _hybris_map_sysconf(int name);

long _hybris_hook_sysconf(int name)
{
    TRACE_HOOK("name %d", name);

    return _hybris_map_sysconf(name);
}

FP_ATTRIB static double _hybris_hook_strtod(const char *nptr, char **endptr)
{
    TRACE_HOOK("nptr '%s' endptr %p", nptr, endptr);

    if (locale_inited == 0) {
            hybris_locale = newlocale(LC_ALL_MASK, "C", 0);
            locale_inited = 1;
    }

    return strtod_l(nptr, endptr, hybris_locale);
}

static long int _hybris_hook_strtol(const char* str, char** endptr, int base)
{
    TRACE_HOOK("str '%s' endptr %p base %i", str, endptr, base);

    return strtol(str, endptr, base);
}

static int _hybris_hook___system_property_read(const void *pi, char *name, char *value)
{
    TRACE_HOOK("pi %p name '%s' value '%s'", pi, name, value);

    return my_property_get(name, value, NULL);
}

static int _hybris_hook___system_property_foreach(void (*propfn)(const void *pi, void *cookie), void *cookie)
{
    TRACE_HOOK("propfn %p cookie %p", propfn, cookie);

    return 0;
}

static const void *_hybris_hook___system_property_find(const char *name)
{
    TRACE_HOOK("name '%s'", name);

    return NULL;
}

static unsigned int _hybris_hook___system_property_serial(const void *pi)
{
    TRACE_HOOK("pi %p", pi);

    return 0;
}

static int _hybris_hook___system_property_wait(const void *pi)
{
    TRACE_HOOK("pi %p", pi);

    return 0;
}

static int _hybris_hook___system_property_update(void *pi, const char *value, unsigned int len)
{
    TRACE_HOOK("pi %p value '%s' len %u", pi, value, len);

    return 0;
}

static int _hybris_hook___system_property_add(const char *name, unsigned int namelen, const char *value, unsigned int valuelen)
{
    TRACE_HOOK("name '%s' namelen %u value '%s' valuelen %u",
               name, namelen, value, valuelen);
    return 0;
}

static unsigned int _hybris_hook___system_property_wait_any(unsigned int serial)
{
    TRACE_HOOK("serial %u", serial);

    return 0;
}

static const void *_hybris_hook___system_property_find_nth(unsigned n)
{
    TRACE_HOOK("n %u", n);

    return NULL;
}

/**
 * NOTE: Normally we don't have to wrap __system_property_get (libc.so) as it is only used
 * through the property_get (libcutils.so) function. However when property_get is used
 * internally in libcutils.so we don't have any chance to hook our replacement in.
 * Therefore we have to hook __system_property_get too and just replace it with the
 * implementation of our internal property handling
 */

int _hybris_hook___system_property_get(const char *name, const char *value)
{
    TRACE_HOOK("name '%s' value '%s'", name, value);

    return my_property_get(name, (char*) value, NULL);
}

int _hybris_hook_property_get(const char *key, char *value, const char *default_value)
{
    TRACE_HOOK("key '%s' value '%s' default value '%s'",
               key, value, default_value);

    return my_property_get(key, value, default_value);
}

int _hybris_hook_property_list(void (*propfn)(const char *key, const char *value, void *cookie), void *cookie)
{
    TRACE_HOOK("propfn '%p' cookie '%p'",
               propfn, cookie);

    return my_property_list(propfn, cookie);
}

int _hybris_hook_property_set(const char *key, const char *value)
{
    TRACE_HOOK("key '%s' value '%s'", key, value);

    return my_property_set(key, value);
}

char *_hybris_hook_getenv(const char *name)
{
    TRACE_HOOK("name '%s'", name);

    return getenv(name);
}

int _hybris_hook_setenv(const char *name, const char *value, int overwrite)
{
    TRACE_HOOK("name '%s' value '%s' overwrite %d", name, value, overwrite);

    return setenv(name, value, overwrite);
}

int _hybris_hook_putenv(char *string)
{
    TRACE_HOOK("string '%s'", string);

    return putenv(string);
}

int _hybris_hook_clearenv(void)
{
    TRACE_HOOK("");

    return clearenv();
}

extern int __cxa_atexit(void (*)(void*), void*, void*);
extern void __cxa_finalize(void * d);
extern int __cxa_thread_atexit(void (*dtor)(void *), void *obj,
                               void *dso_symbol);

struct open_redirect {
    const char *from;
    const char *to;
};

struct open_redirect open_redirects[] = {
    { "/dev/log/main", "/dev/alog/main" },
    { "/dev/log/radio", "/dev/alog/radio" },
    { "/dev/log/system", "/dev/alog/system" },
    { "/dev/log/events", "/dev/alog/events" },
    { NULL, NULL }
};

int _hybris_hook_open(const char *pathname, int flags, ...)
{
    va_list ap;
    mode_t mode = 0;
    const char *target_path = pathname;

    TRACE_HOOK("pathname '%s' flags %d", pathname, flags);

    if (pathname != NULL) {
            struct open_redirect *entry = &open_redirects[0];
            while (entry->from != NULL) {
                    if (strcmp(pathname, entry->from) == 0) {
                            target_path = entry->to;
                            break;
                    }
                    entry++;
            }
    }

    if (flags & O_CREAT) {
            va_start(ap, flags);
            mode = va_arg(ap, mode_t);
            va_end(ap);
    }

    return open(target_path, flags, mode);
}

/**
 * Wrap some GCC builtin functions, which don't have any address
 */
__THROW int _hybris_hook___sprintf_chk (char *__restrict __s, int __flag, size_t __slen,
			  const char *__restrict __format, ...)
{
    int ret = 0;
    va_list args;
    va_start(args,__format);
    ret = __vsprintf_chk (__s, __flag, __slen, __format, args);
    va_end(args);

    return ret;
}
__THROW int _hybris_hook___snprintf_chk (char *__restrict __s, size_t __n, int __flag,
			   size_t __slen, const char *__restrict __format, ...)
{
    int ret = 0;
    va_list args;
    va_start(args,__format);
    ret = __vsnprintf_chk (__s, __n, __flag, __slen, __format, args);
    va_end(args);

    return ret;
}

struct __wrapped_atexit {
    void (*dtor)(void *);
    void *obj;
    void *dso_handle;
};

static void __dtor_wrapper(void *obj) {
    struct __wrapped_atexit *wrapped = obj;
    /* Call the wrapped dtor. */
    wrapped->dtor(wrapped->obj);
    /* Reduce dso_handle's ref count. */
    __hybris_remove_thread_local_dtor(wrapped->dso_handle);
    /* Free the wrapper. */
    free(wrapped);
}

extern const void * const __dso_handle;

static int _hybris_hook___cxa_thread_atexit(void (*dtor)(void *), void *obj,
                                            void *dso_symbol)
{
    struct __wrapped_atexit *wrapped = malloc(sizeof(struct __wrapped_atexit));
    wrapped->dtor = dtor;
    wrapped->obj = obj;
    wrapped->dso_handle = dso_symbol;

    /* Call Glibc's implementation. Pass our symbol to prevent ourself from
     * being unloaded. */
    int ret;
    if ((ret = __cxa_thread_atexit(__dtor_wrapper, wrapped, &__dso_handle)) != 0) {
        free(wrapped);
        return ret;
    }

    /* Increase refcount of this dso_symbol. */
    __hybris_add_thread_local_dtor(dso_symbol);

    return ret;
}

int _hybris_hook_prctl(int option, unsigned long arg2, unsigned long arg3,
             unsigned long arg4, unsigned long arg5)
{
    TRACE_HOOK("option %d arg2 %lu arg3 %lu arg4 %lu arg5 %lu",
               option, arg2, arg3, arg4, arg5);

#ifdef MALI_QUIRKS
    if (option == PR_SET_NAME) {
        char *name = (char*) arg2;

        if (strcmp(name, MALI_HIST_DUMP_THREAD_NAME) == 0) {

            // This can only work because prctl with PR_SET_NAME
            // can be only called for the current thread and not
            // for another thread so we can safely pause things.

            HYBRIS_DEBUG_LOG(HOOKS, "%s: Found mali-hist-dump, killing thread ...",
                             __FUNCTION__);

            pthread_exit(NULL);
        }
    }
#endif

    return prctl(option, arg2, arg3, arg4, arg5);
}

static char* _hybris_hook_basename(const char *path)
{
    char buf[PATH_MAX];

    TRACE_HOOK("path '%s'", path);

    memset(buf, 0, sizeof(buf));

    if (path)
        strncpy(buf, path, sizeof(buf));

    buf[sizeof buf - 1] = '\0';

    return basename(buf);
}

static char* _hybris_hook_dirname(char *path)
{
    char buf[PATH_MAX];

    TRACE_HOOK("path '%s'", path);

    memset(buf, 0, sizeof(buf));

    if (path)
        strncpy(buf, path, sizeof(buf));

    buf[sizeof buf - 1] = '\0';

    return dirname(path);
}

static char* _hybris_hook_strerror(int errnum)
{
    TRACE_HOOK("errnum %d", errnum);

    return strerror(errnum);
}

static char* _hybris_hook__gnu_strerror_r(int errnum, char *buf, size_t buf_len)
{
    TRACE_HOOK("errnum %d buf '%s' buf len %zu", errnum, buf, buf_len);

    return strerror_r(errnum, buf, buf_len);
}

static int _hybris_hook_mprotect(void *addr, size_t len, int prot)
{
    TRACE_HOOK("addr %p len %zu prot %d", addr, len, prot);

    return mprotect(addr, len, prot);
}

static int _hybris_hook_posix_memalign(void **memptr, size_t alignment, size_t size)
{
    TRACE_HOOK("memptr %p alignment %zu size %zu", memptr, alignment, size);

    return posix_memalign(memptr, alignment, size);
}

static pid_t _hybris_hook_fork(void)
{
    TRACE_HOOK("");

    return fork();
}

static locale_t _hybris_hook_newlocale(int category_mask, const char *locale, locale_t base)
{
    TRACE_HOOK("category mask %i locale '%s'", category_mask, locale);

    return newlocale(category_mask, locale, base);
}

static void _hybris_hook_freelocale(locale_t locobj)
{
    TRACE_HOOK("");

    return freelocale(locobj);
}

static locale_t _hybris_hook_duplocale(locale_t locobj)
{
    TRACE_HOOK("");

    return duplocale(locobj);
}

static locale_t _hybris_hook_uselocale(locale_t newloc)
{
    TRACE_HOOK("");

    return uselocale(newloc);
}

static struct lconv* _hybris_hook_localeconv(void)
{
    TRACE_HOOK("");

    return localeconv();
}

static char* _hybris_hook_setlocale(int category, const char *locale)
{
    TRACE_HOOK("category %i locale '%s'", category, locale);

    return setlocale(category, locale);
}

static void* _hybris_hook_mmap(void *addr, size_t len, int prot,
                  int flags, int fd, bionic_off_t offset)
{
    TRACE_HOOK("addr %p len %zu prot %i flags %i fd %i offset %ld",
               addr, len, prot, flags, fd, offset);

    return mmap(addr, len, prot, flags, fd, offset);
}

static void* _hybris_hook_mmap64(void *addr, size_t len, int prot,
                  int flags, int fd, off64_t offset)
{
    TRACE_HOOK("addr %p len %zu prot %i flags %i fd %i offset %ld",
               addr, len, prot, flags, fd, offset);

    return mmap64(addr, len, prot, flags, fd, offset);
}

static int _hybris_hook_munmap(void *addr, size_t length)
{
    TRACE_HOOK("addr %p length %zu", addr, length);

    return munmap(addr, length);
}

extern size_t strlcat(char *dst, const char *src, size_t siz);
extern size_t strlcpy(char *dst, const char *src, size_t siz);

static int _hybris_hook_strcmp(const char *s1, const char *s2)
{
    TRACE_HOOK("s1 '%s' s2 '%s'", s1, s2);

    if ( s1 == NULL || s2 == NULL)
        return -1;

    return strcmp(s1, s2);
}

static void *_hybris_hook_dlopen(const char *filename, int flag)
{
    TRACE("filename %s flag %i", filename, flag);

    return _android_dlopen(filename,flag);
}

static void *_hybris_hook_dlsym(void *handle, const char *symbol)
{
    TRACE("handle %p symbol %s", handle, symbol);

    void *hook = hybris_get_hooked_symbol(symbol, "android_dlsym");

    if (hook) {
        return hook;
    }

    return _android_dlsym(handle,symbol);
}

static void *_hybris_hook_dlvsym(void *handle, const char *symbol, const char* version)
{
    TRACE("handle %p symbol %s version %s", handle, symbol, version);

    return _android_dlvsym(handle,symbol,version);
}

static void* _hybris_hook_dladdr(void *addr, Dl_info *info)
{
    TRACE("addr %p info %p", addr, info);

    return (void *)_android_dladdr(addr, info);
}

static int _hybris_hook_dlclose(void *handle)
{
    TRACE("handle %p", handle);

    return _android_dlclose(handle);
}

static const char *_hybris_hook_dlerror(void)
{
    TRACE("");

    /* Call the linker entry, not android_dlerror(). The wrapper would
     * re-enter linker init and deadlock if this hook ran under pthread_once. */
    return _android_dlerror ? _android_dlerror() : NULL;
}

void *_hybris_hook_dl_unwind_find_exidx(void* pc, int* pcount)
{
    TRACE("pc %p, pcount %p", pc, pcount);

    return _android_dl_unwind_find_exidx(pc, pcount);
}

int _hybris_hook_dl_iterate_phdr(int (*cb)(void* info, size_t size, void* data), void* data)
{
    TRACE("cb %p, data %p", cb, data);

    return _android_dl_iterate_phdr(cb, data);
}

void _hybris_hook_android_get_LD_LIBRARY_PATH(char* buffer, size_t buffer_size)
{
    TRACE("buffer %p, buffer_size %zu\n", buffer, buffer_size);

    _android_get_LD_LIBRARY_PATH(buffer, buffer_size);
}

void _hybris_hook_android_update_LD_LIBRARY_PATH(const char* ld_library_path)
{
    TRACE("ld_library_path %s", ld_library_path);

    _android_update_LD_LIBRARY_PATH(ld_library_path);
}

void* _hybris_hook_android_dlopen_ext(const char* filename, int flag, const void* extinfo)
{
    TRACE("filename %s, flag %d, extinfo %p", filename, flag, extinfo);

    return _android_dlopen_ext(filename, flag, extinfo);
}

void _hybris_hook_android_set_application_target_sdk_version(uint32_t target)
{
    TRACE("target %u", target);

    _android_set_application_target_sdk_version(target);
}

int _hybris_hook_android_get_application_target_sdk_version()
{
    TRACE("");

    return _android_get_application_target_sdk_version();
}

void* _hybris_hook_android_create_namespace(const char* name,
                                                     const char* ld_library_path,
                                                     const char* default_library_path,
                                                     uint64_t type,
                                                     const char* permitted_when_isolated_path,
                                                     void* parent)
{
    TRACE("name %s, ld_library_path %s, default_library_path %s, type %" PRIu64 ", permitted_when_isolated_path %s, parent %p", name, ld_library_path, default_library_path, type, permitted_when_isolated_path, parent);

    return _android_create_namespace(name, ld_library_path, default_library_path, type, permitted_when_isolated_path, parent);
}
#if WANT_LINKER_Q
void* _hybris_hook___loader_shared_globals()
{
    TRACE("");

    return _android_shared_globals();
}
#endif
bool _hybris_hook_android_init_anonymous_namespace(const char* shared_libs_sonames,
                                      const char* library_search_path)
{
    TRACE("shared_libs_sonames %s, library_search_path %s", shared_libs_sonames, library_search_path);

    return _android_init_anonymous_namespace(shared_libs_sonames, library_search_path);
}

void _hybris_hook_android_dlwarning(void* obj, void (*f)(void*, const char*))
{
    TRACE("obj %p, f %p", obj, f);

    _android_dlwarning(obj, f);
}

void* _hybris_hook_android_get_exported_namespace(const char* name)
{
    TRACE("name %s", name);

    return _android_get_exported_namespace(name);
}

void _hybris_hook_free(void *ptr)
{
    TRACE_HOOK("ptr %p", ptr);
    free(ptr);
}

#if !defined(cfree)
#define cfree free
#endif

int _hybris_hook_android_fdsan_set_error_level(int new_level)
{
    TRACE_HOOK("new_level %d", new_level);
    return new_level;
}

void _hybris_hook_android_fdsan_exchange_owner_tag(int fd, uint64_t expected_tag, uint64_t new_tag)
{
    (void)expected_tag;
    (void)new_tag;
    TRACE_HOOK("fd %d", fd);
}

int _hybris_hook_android_fdsan_close_with_tag(int fd, uint64_t tag)
{
    (void)tag;
    TRACE_HOOK("fd %d", fd);
    return close(fd);
}

/* Bionic libdl.so's __cfi_slowpath{,_diag} validate indirect calls against a
 * sparse shadow table that the bionic linker builds at process start, covering
 * only system libraries. Vendor libraries we android_dlopen here are absent
 * from that shadow, so the real __cfi_slowpath would fault on their indirect
 * calls. Hooking the symbol redirects each loaded vendor library's GOT entry
 * for __cfi_slowpath{,_diag} at relocation time (linker.cpp:_get_hooked_symbol)
 * to these no-op stubs, matching kUncheckedShadow semantics — vendor libraries
 * lack __cfi_check anyway, so a properly initialised shadow would pass them
 * through unchecked. Avoids ever needing to mprotect bionic libdl.so, which
 * fails on stock Android's runas_app SELinux domain (no system_file:execmod). */
static void _hybris_hook___cfi_slowpath(uint64_t CallSiteTypeId, void *TargetAddr)
{
    (void)CallSiteTypeId;
    (void)TargetAddr;
}

static void _hybris_hook___cfi_slowpath_diag(uint64_t CallSiteTypeId, void *TargetAddr, void *DiagData)
{
    (void)CallSiteTypeId;
    (void)TargetAddr;
    (void)DiagData;
}

// old property hooks for pre-android 8 approach
static struct _hook hooks_properties[] = {
    HOOK_INDIRECT(property_get),
    HOOK_INDIRECT(property_set),
    HOOK_INDIRECT(property_list),
    HOOK_INDIRECT(__system_property_get),
    HOOK_INDIRECT(__system_property_read),
    HOOK_TO(__system_property_set, _hybris_hook_property_set),
    HOOK_INDIRECT(__system_property_foreach),
    HOOK_INDIRECT(__system_property_find),
    HOOK_INDIRECT(__system_property_serial),
    HOOK_INDIRECT(__system_property_wait),
    HOOK_INDIRECT(__system_property_update),
    HOOK_INDIRECT(__system_property_add),
    HOOK_INDIRECT(__system_property_wait_any),
    HOOK_INDIRECT(__system_property_find_nth),
};

static struct _hook hooks_common[] = {

    HOOK_DIRECT(getenv),
    HOOK_DIRECT_NO_DEBUG(printf),
    HOOK_INDIRECT(malloc),
    HOOK_INDIRECT(aligned_alloc),
    HOOK_INDIRECT(free),
    HOOK_DIRECT_NO_DEBUG(calloc),
    HOOK_DIRECT_NO_DEBUG(free),
    HOOK_DIRECT_NO_DEBUG(realloc),
    HOOK_DIRECT_NO_DEBUG(memalign),
    HOOK_DIRECT_NO_DEBUG(valloc),
    HOOK_DIRECT_NO_DEBUG(pvalloc),
    HOOK_DIRECT(fread),
    HOOK_DIRECT_NO_DEBUG(getxattr),
    HOOK_DIRECT(mprotect),
    /* string.h */
    HOOK_DIRECT_NO_DEBUG(memccpy),
    HOOK_DIRECT_NO_DEBUG(memchr),
    HOOK_DIRECT_NO_DEBUG(memrchr),
    HOOK_DIRECT(memcmp),
    HOOK_INDIRECT(memcpy),
    HOOK_DIRECT_NO_DEBUG(memmove),
    HOOK_DIRECT_NO_DEBUG(memset),
    HOOK_DIRECT_NO_DEBUG(memmem),
    HOOK_DIRECT_NO_DEBUG(getlogin),
    // HOOK_DIRECT(memswap),
    HOOK_DIRECT_NO_DEBUG(index),
    HOOK_DIRECT_NO_DEBUG(rindex),
    HOOK_DIRECT_NO_DEBUG(stpcpy),
    HOOK_DIRECT_NO_DEBUG(stpncpy),
    HOOK_DIRECT_NO_DEBUG(strchr),
    HOOK_DIRECT_NO_DEBUG(strrchr),
    HOOK_INDIRECT(strlen),
    HOOK_INDIRECT(strcmp),
    HOOK_DIRECT_NO_DEBUG(strcpy),
    HOOK_DIRECT_NO_DEBUG(strcat),
    HOOK_DIRECT_NO_DEBUG(strcasecmp),
    HOOK_DIRECT_NO_DEBUG(strncasecmp),
    HOOK_DIRECT_NO_DEBUG(strdup),
    HOOK_DIRECT_NO_DEBUG(strstr),
    HOOK_DIRECT_NO_DEBUG(strtok),
    HOOK_DIRECT_NO_DEBUG(strtok_r),
    HOOK_DIRECT(strerror),
    HOOK_DIRECT_NO_DEBUG(strerror_r),
    HOOK_DIRECT_NO_DEBUG(strnlen),
    HOOK_DIRECT_NO_DEBUG(strncat),
    HOOK_DIRECT_NO_DEBUG(strndup),
    HOOK_DIRECT_NO_DEBUG(strncmp),
    HOOK_DIRECT_NO_DEBUG(strncpy),
    HOOK_INDIRECT(strtod),
    HOOK_DIRECT_NO_DEBUG(strcspn),
    HOOK_DIRECT_NO_DEBUG(strpbrk),
    HOOK_DIRECT_NO_DEBUG(strsep),
    HOOK_DIRECT_NO_DEBUG(strspn),
    HOOK_DIRECT_NO_DEBUG(strsignal),
    HOOK_DIRECT_NO_DEBUG(getgrnam),
    HOOK_DIRECT_NO_DEBUG(strcoll),
    HOOK_DIRECT_NO_DEBUG(strxfrm),
    /* strings.h */
    HOOK_DIRECT_NO_DEBUG(bcmp),
    HOOK_DIRECT_NO_DEBUG(bcopy),
    HOOK_DIRECT_NO_DEBUG(bzero),
    HOOK_DIRECT_NO_DEBUG(ffs),
    HOOK_INDIRECT(__sprintf_chk),
    HOOK_INDIRECT(__snprintf_chk),
    /* pthread.h */
    HOOK_DIRECT_NO_DEBUG(getauxval),
    HOOK_INDIRECT(gettid),
    HOOK_DIRECT_NO_DEBUG(getpid),
    HOOK_DIRECT_NO_DEBUG(pthread_atfork),
    HOOK_INDIRECT(pthread_create),
    HOOK_INDIRECT(pthread_kill),
    HOOK_DIRECT_NO_DEBUG(pthread_exit),
    HOOK_DIRECT_NO_DEBUG(pthread_join),
    HOOK_DIRECT_NO_DEBUG(pthread_detach),
    HOOK_DIRECT_NO_DEBUG(pthread_self),
    HOOK_DIRECT_NO_DEBUG(pthread_equal),
    HOOK_DIRECT_NO_DEBUG(pthread_getschedparam),
    HOOK_DIRECT_NO_DEBUG(pthread_setschedparam),
    HOOK_INDIRECT(pthread_mutex_init),
    HOOK_INDIRECT(pthread_mutex_destroy),
    HOOK_INDIRECT(pthread_mutex_lock),
    HOOK_INDIRECT(pthread_mutex_unlock),
    HOOK_INDIRECT(pthread_mutex_trylock),
    HOOK_INDIRECT(pthread_mutex_lock_timeout_np),
    HOOK_INDIRECT(pthread_mutex_timedlock),
    HOOK_INDIRECT(pthread_mutex_timedlock_monotonic_np),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_init),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_destroy),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_gettype),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_settype),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_getpshared),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_getprotocol),
    HOOK_DIRECT_NO_DEBUG(pthread_mutexattr_setprotocol),
    HOOK_DIRECT(pthread_mutexattr_setpshared),
    HOOK_DIRECT_NO_DEBUG(pthread_condattr_init),
    HOOK_DIRECT_NO_DEBUG(pthread_condattr_getpshared),
    HOOK_DIRECT_NO_DEBUG(pthread_condattr_setpshared),
    HOOK_DIRECT_NO_DEBUG(pthread_condattr_destroy),
    HOOK_DIRECT_NO_DEBUG(pthread_condattr_getclock),
    HOOK_DIRECT_NO_DEBUG(pthread_condattr_setclock),
    HOOK_INDIRECT(pthread_cond_init),
    HOOK_INDIRECT(pthread_cond_destroy),
    HOOK_INDIRECT(pthread_cond_broadcast),
    HOOK_INDIRECT(pthread_cond_signal),
    HOOK_INDIRECT(pthread_cond_wait),
    HOOK_INDIRECT(pthread_cond_clockwait),
    HOOK_INDIRECT(pthread_cond_timedwait),
    HOOK_TO(pthread_cond_timedwait_monotonic, _hybris_hook_pthread_cond_timedwait_monotonic),
    HOOK_TO(pthread_cond_timedwait_monotonic_np, _hybris_hook_pthread_cond_timedwait_monotonic),
    HOOK_INDIRECT(pthread_cond_timedwait_relative_np),
    HOOK_DIRECT_NO_DEBUG(pthread_key_delete),
    HOOK_DIRECT_NO_DEBUG(pthread_getname_np),
    HOOK_INDIRECT(pthread_setname_np),
    HOOK_DIRECT_NO_DEBUG(pthread_once),
    HOOK_DIRECT_NO_DEBUG(pthread_key_create),
    HOOK_DIRECT(pthread_setspecific),
    HOOK_INDIRECT(pthread_getspecific),
    HOOK_INDIRECT(pthread_attr_init),
    HOOK_INDIRECT(pthread_attr_destroy),
    HOOK_INDIRECT(pthread_attr_setdetachstate),
    HOOK_INDIRECT(pthread_attr_getdetachstate),
    HOOK_INDIRECT(pthread_attr_setschedpolicy),
    HOOK_INDIRECT(pthread_attr_getschedpolicy),
    HOOK_INDIRECT(pthread_attr_setschedparam),
    HOOK_INDIRECT(pthread_attr_getschedparam),
    HOOK_INDIRECT(pthread_attr_setstacksize),
    HOOK_INDIRECT(pthread_attr_getstacksize),
    HOOK_INDIRECT(pthread_attr_setstackaddr),
    HOOK_INDIRECT(pthread_attr_getstackaddr),
    HOOK_INDIRECT(pthread_attr_setstack),
    HOOK_INDIRECT(pthread_attr_getstack),
    HOOK_INDIRECT(pthread_attr_setguardsize),
    HOOK_INDIRECT(pthread_attr_getguardsize),
    HOOK_INDIRECT(pthread_attr_setscope),
    HOOK_INDIRECT(pthread_attr_getscope),
    HOOK_INDIRECT(pthread_getattr_np),
    HOOK_INDIRECT(pthread_rwlockattr_init),
    HOOK_INDIRECT(pthread_rwlockattr_destroy),
    HOOK_INDIRECT(pthread_rwlockattr_setpshared),
    HOOK_INDIRECT(pthread_rwlockattr_getpshared),
    HOOK_INDIRECT(pthread_rwlock_init),
    HOOK_INDIRECT(pthread_rwlock_destroy),
    HOOK_INDIRECT(pthread_rwlock_unlock),
    HOOK_INDIRECT(pthread_rwlock_wrlock),
    HOOK_INDIRECT(pthread_rwlock_rdlock),
    HOOK_INDIRECT(pthread_rwlock_tryrdlock),
    HOOK_INDIRECT(pthread_rwlock_trywrlock),
    HOOK_INDIRECT(pthread_rwlock_timedrdlock),
    HOOK_INDIRECT(pthread_rwlock_timedwrlock),
    HOOK_INDIRECT(__pthread_cleanup_push),
    HOOK_INDIRECT(__pthread_cleanup_pop),
    /* bionic-only pthread */
    HOOK_TO(__pthread_gettid, _hybris_hook_pthread_gettid_np),
    HOOK_INDIRECT(pthread_gettid_np),
    /* stdio.h */
    HOOK_TO(__isthreaded, &_hybris_hook___isthreaded),
    HOOK_TO(__sF, _hybris_hook_sF),
    HOOK_DIRECT_NO_DEBUG(fopen),
    HOOK_DIRECT_NO_DEBUG(fdopen),
    HOOK_DIRECT_NO_DEBUG(popen),
    HOOK_DIRECT_NO_DEBUG(puts),
    HOOK_DIRECT_NO_DEBUG(sprintf),
    HOOK_DIRECT_NO_DEBUG(asprintf),
    HOOK_DIRECT_NO_DEBUG(vasprintf),
    HOOK_DIRECT_NO_DEBUG(snprintf),
    HOOK_DIRECT_NO_DEBUG(vsprintf),
    HOOK_DIRECT_NO_DEBUG(vsnprintf),
    HOOK_INDIRECT(clearerr),
    HOOK_INDIRECT(fclose),
    HOOK_INDIRECT(feof),
    HOOK_INDIRECT(ferror),
    HOOK_INDIRECT(fflush),
    HOOK_INDIRECT(fgetc),
    HOOK_INDIRECT(fgetpos),
    HOOK_INDIRECT(fgets),
    HOOK_INDIRECT(fprintf),
    HOOK_INDIRECT(fputc),
    HOOK_INDIRECT(fputs),
    HOOK_INDIRECT(fread),
    HOOK_INDIRECT(freopen),
    HOOK_INDIRECT(fscanf),
    HOOK_INDIRECT(fseek),
    HOOK_INDIRECT(fseeko),
    HOOK_INDIRECT(fsetpos),
    HOOK_INDIRECT(ftell),
    HOOK_INDIRECT(ftello),
    HOOK_INDIRECT(fwrite),
    HOOK_INDIRECT(getc),
    HOOK_INDIRECT(getdelim),
    HOOK_INDIRECT(getline),
    HOOK_INDIRECT(putc),
    HOOK_INDIRECT(rewind),
    HOOK_INDIRECT(setbuf),
    HOOK_INDIRECT(setvbuf),
    HOOK_INDIRECT(ungetc),
    HOOK_INDIRECT(vfprintf),
    HOOK_INDIRECT(vfscanf),
    HOOK_INDIRECT(fileno),
    HOOK_INDIRECT(pclose),
    HOOK_INDIRECT(flockfile),
    HOOK_INDIRECT(ftrylockfile),
    HOOK_INDIRECT(funlockfile),
    HOOK_INDIRECT(clearerr_unlocked),
    HOOK_INDIRECT(feof_unlocked),
    HOOK_INDIRECT(ferror_unlocked),
    HOOK_INDIRECT(getc_unlocked),
    HOOK_INDIRECT(putc_unlocked),
    //HOOK(fgetln),
    HOOK_INDIRECT(fpurge),
    HOOK_INDIRECT(getw),
    HOOK_INDIRECT(putw),
    HOOK_INDIRECT(setbuffer),
    HOOK_INDIRECT(setlinebuf),
    HOOK_TO(__errno, __errno_location),
    HOOK_INDIRECT(__set_errno),
    HOOK_TO(__set_errno_internal, _hybris_hook___set_errno),
    HOOK_TO(__progname, &program_invocation_name),
    /* net specifics, to avoid __res_get_state */
    HOOK_INDIRECT(getaddrinfo),
    HOOK_INDIRECT(freeaddrinfo),
    HOOK_DIRECT_NO_DEBUG(gethostbyaddr),
    HOOK_DIRECT_NO_DEBUG(gethostbyname),
    HOOK_DIRECT_NO_DEBUG(gethostbyname2),
    HOOK_DIRECT_NO_DEBUG(gethostent),
    HOOK_DIRECT_NO_DEBUG(strftime),
    HOOK_INDIRECT(sysconf),
    HOOK_INDIRECT(dlopen),
    HOOK_INDIRECT(dlerror),
    HOOK_INDIRECT(dlsym),
    HOOK_INDIRECT(dlvsym),
    HOOK_INDIRECT(dladdr),
    HOOK_INDIRECT(dlclose),
    HOOK_INDIRECT(dl_unwind_find_exidx),
    HOOK_INDIRECT(dl_iterate_phdr),
    HOOK_INDIRECT(android_get_LD_LIBRARY_PATH),
    HOOK_INDIRECT(android_update_LD_LIBRARY_PATH),
    HOOK_INDIRECT(android_dlopen_ext),
    HOOK_INDIRECT(android_set_application_target_sdk_version),
    HOOK_INDIRECT(android_get_application_target_sdk_version),
    HOOK_INDIRECT(android_create_namespace),
    HOOK_INDIRECT(android_init_anonymous_namespace),
    HOOK_INDIRECT(android_dlwarning),
    HOOK_INDIRECT(android_get_exported_namespace),
#if WANT_LINKER_Q
    HOOK_INDIRECT(__loader_shared_globals),
#endif
    /* dirent.h */
    HOOK_DIRECT_NO_DEBUG(opendir),
    HOOK_DIRECT_NO_DEBUG(fdopendir),
    HOOK_DIRECT_NO_DEBUG(closedir),
    HOOK_DIRECT_NO_DEBUG(__fsetlocking),
    HOOK_INDIRECT(readdir),
    HOOK_INDIRECT(readdir_r),
    HOOK_DIRECT_NO_DEBUG(rewinddir),
    HOOK_DIRECT_NO_DEBUG(seekdir),
    HOOK_DIRECT_NO_DEBUG(telldir),
    HOOK_DIRECT_NO_DEBUG(dirfd),
    HOOK_INDIRECT(scandir),
    HOOK_INDIRECT(alphasort),
    HOOK_INDIRECT(versionsort),
    /* fcntl.h */
    HOOK_INDIRECT(open),
    HOOK_INDIRECT(__get_tls_hooks),
    HOOK_DIRECT_NO_DEBUG(sscanf),
    HOOK_DIRECT_NO_DEBUG(scanf),
    HOOK_DIRECT_NO_DEBUG(vscanf),
    HOOK_DIRECT_NO_DEBUG(vsscanf),
    HOOK_DIRECT_NO_DEBUG(openlog),
    HOOK_DIRECT_NO_DEBUG(syslog),
    HOOK_DIRECT_NO_DEBUG(closelog),
    HOOK_DIRECT_NO_DEBUG(vsyslog),
    HOOK_DIRECT_NO_DEBUG(timer_create),
    HOOK_DIRECT_NO_DEBUG(timer_settime),
    HOOK_DIRECT_NO_DEBUG(timer_gettime),
    HOOK_DIRECT_NO_DEBUG(timer_delete),
    HOOK_DIRECT_NO_DEBUG(timer_getoverrun),
    HOOK_DIRECT_NO_DEBUG(localtime),
    HOOK_DIRECT_NO_DEBUG(localtime_r),
    HOOK_DIRECT_NO_DEBUG(gmtime),
    HOOK_DIRECT_NO_DEBUG(abort),
    HOOK_DIRECT_NO_DEBUG(writev),
    /* unistd.h */
    HOOK_DIRECT_NO_DEBUG(access),
    /* grp.h */
    HOOK_DIRECT_NO_DEBUG(getgrgid),
    /* C++ ABI */
    HOOK_DIRECT_NO_DEBUG(__cxa_atexit),
    HOOK_DIRECT_NO_DEBUG(__cxa_finalize),
    HOOK_INDIRECT(__cxa_thread_atexit),
    /* sys/prctl.h */
    HOOK_INDIRECT(prctl),
    /* stdio_ext.h */
    HOOK_INDIRECT(__fbufsize),
    HOOK_INDIRECT(__fpending),
    HOOK_INDIRECT(__flbf),
    HOOK_INDIRECT(__freadable),
    HOOK_INDIRECT(__fwritable),
    HOOK_INDIRECT(__freading),
    HOOK_INDIRECT(__fwriting),
    HOOK_INDIRECT(__fsetlocking),
    HOOK_INDIRECT(_flushlbf),
    HOOK_INDIRECT(__fpurge),
};

static struct _hook hooks_mm[] = {
    /* CFI was added in Android M (API 23). See cfi stub definitions above. */
    HOOK_TO(__cfi_slowpath, _hybris_hook___cfi_slowpath),
    HOOK_TO(__cfi_slowpath_diag, _hybris_hook___cfi_slowpath_diag),
    HOOK_DIRECT(strtol),
    HOOK_DIRECT_NO_DEBUG(strlcat),
    HOOK_DIRECT_NO_DEBUG(strlcpy),
    HOOK_DIRECT(setenv),
    HOOK_DIRECT(putenv),
    HOOK_DIRECT(clearenv),
    HOOK_DIRECT_NO_DEBUG(dprintf),
    HOOK_DIRECT_NO_DEBUG(mallinfo),
    HOOK_DIRECT(malloc_usable_size),
    HOOK_DIRECT(posix_memalign),
    HOOK_DIRECT(mprotect),
    HOOK_TO(__gnu_strerror_r, _hybris_hook__gnu_strerror_r),
    HOOK_INDIRECT(pthread_rwlockattr_getkind_np),
    HOOK_INDIRECT(pthread_rwlockattr_setkind_np),
    /* unistd.h */
    HOOK_DIRECT(fork),
    HOOK_DIRECT_NO_DEBUG(ttyname),
    HOOK_DIRECT_NO_DEBUG(swprintf),
    HOOK_DIRECT_NO_DEBUG(fmemopen),
    HOOK_DIRECT_NO_DEBUG(open_memstream),
    HOOK_DIRECT_NO_DEBUG(open_wmemstream),
    HOOK_DIRECT_NO_DEBUG(ptsname),
    HOOK_TO(__hybris_set_errno_internal, _hybris_hook___set_errno),
    HOOK_DIRECT_NO_DEBUG(getservbyname),
    HOOK_DIRECT_NO_DEBUG(close), /* avoid calling fdsan functions */
    /* libgen.h */
    HOOK_INDIRECT(basename),
    HOOK_INDIRECT(dirname),
    /* locale.h */
    HOOK_DIRECT(newlocale),
    HOOK_DIRECT(freelocale),
    HOOK_DIRECT(duplocale),
    HOOK_DIRECT(uselocale),
    HOOK_DIRECT(localeconv),
    HOOK_DIRECT(setlocale),
    /* sys/mman.h */
#if defined(LP64)
    HOOK_DIRECT(mmap),
#else
    HOOK_INDIRECT(mmap),
#endif
    HOOK_DIRECT(mmap64),
    HOOK_DIRECT(munmap),
    /* wchar.h */
    HOOK_DIRECT_NO_DEBUG(wmemchr),
    HOOK_DIRECT_NO_DEBUG(wmemcmp),
    HOOK_DIRECT_NO_DEBUG(wmemcpy),
    HOOK_DIRECT_NO_DEBUG(wmemmove),
    HOOK_DIRECT_NO_DEBUG(wmemset),
    HOOK_DIRECT_NO_DEBUG(wmempcpy),
    HOOK_INDIRECT(fputws),
    // It's enough to hook vfwprintf here as fwprintf will call it with a
    // proper va_list in place so we don't have to handle this here.
    HOOK_INDIRECT(vfwprintf),
    HOOK_INDIRECT(fputwc),
    HOOK_INDIRECT(putwc),
    HOOK_INDIRECT(fgetwc),
    HOOK_INDIRECT(getwc),
    /* sched.h */
    HOOK_DIRECT_NO_DEBUG(clone),
    /* mntent.h */
    HOOK_DIRECT(setmntent),
    HOOK_INDIRECT(getmntent),
    HOOK_INDIRECT(getmntent_r),
    HOOK_INDIRECT(endmntent),
    /* stdlib.h */
    HOOK_DIRECT_NO_DEBUG(system),
    /* pwd.h */
    HOOK_DIRECT_NO_DEBUG(getpwuid),
    HOOK_DIRECT_NO_DEBUG(getpwnam),
    /* signal.h */
    /* Hooks commented out for the moment as we need proper translations between
     * bionic and glibc types for them to work (for instance, sigset_t has
     * different definitions in each library).
     */
#if 0
    HOOK_INDIRECT(sigaction),
    HOOK_INDIRECT(sigaddset),
    HOOK_INDIRECT(sigaltstack),
    HOOK_INDIRECT(sigblock),
    HOOK_INDIRECT(sigdelset),
    HOOK_INDIRECT(sigemptyset),
    HOOK_INDIRECT(sigfillset),
    HOOK_INDIRECT(siginterrupt),
    HOOK_INDIRECT(sigismember),
    HOOK_INDIRECT(siglongjmp),
    HOOK_INDIRECT(signal),
    HOOK_INDIRECT(signalfd),
    HOOK_INDIRECT(sigpending),
    HOOK_INDIRECT(sigprocmask),
    HOOK_INDIRECT(sigqueue),
    // setjmp.h defines segsetjmp via a #define and the real symbol
    // we have to forward to is __sigsetjmp
    {"sigsetjmp", __sigsetjmp},
    HOOK_INDIRECT(sigsetmask),
    HOOK_INDIRECT(sigsuspend),
    HOOK_INDIRECT(sigtimedwait),
    HOOK_INDIRECT(sigwait),
    HOOK_INDIRECT(sigwaitinfo),
#endif
    /* dirent.h */
    HOOK_TO(readdir64, _hybris_hook_readdir),
    HOOK_TO(readdir64_r, _hybris_hook_readdir_r),
    HOOK_INDIRECT(scandir),
    HOOK_TO(scandir64, _hybris_hook_scandir),
};

static struct _hook hooks_n[] = {
    /* stdio.h */
    HOOK_INDIRECT(fgetpos64),
    HOOK_INDIRECT(fsetpos64),
    HOOK_INDIRECT(fseeko64),
    HOOK_INDIRECT(ftello64),
    HOOK_DIRECT_NO_DEBUG(fopen64),
    HOOK_INDIRECT(freopen64),
    HOOK_INDIRECT(fileno_unlocked),
    /* dirent.h */
    HOOK_INDIRECT(scandirat),
    HOOK_TO(scandirat64, _hybris_hook_scandirat),
};

static struct _hook hooks_p[] = {
    /* stdio.h */
    HOOK_INDIRECT(fflush_unlocked),
    HOOK_INDIRECT(fputc_unlocked),
    HOOK_INDIRECT(fread_unlocked),
    HOOK_INDIRECT(fgetc_unlocked),
    HOOK_INDIRECT(fwrite_unlocked),
    HOOK_INDIRECT(fgets_unlocked),
    HOOK_INDIRECT(fputs_unlocked),
    /* fdsan.h */
    HOOK_INDIRECT(android_fdsan_set_error_level),
    HOOK_INDIRECT(android_fdsan_exchange_owner_tag),
    HOOK_INDIRECT(android_fdsan_close_with_tag),
    /* pthread.h */
    HOOK_DIRECT_NO_DEBUG(pthread_setschedprio),
};

static int hook_cmp(const void *a, const void *b)
{
    return strcmp(((struct _hook*)a)->name, ((struct _hook*)b)->name);
}

void hybris_set_hook_callback(hybris_hook_cb callback)
{
    hook_callback = callback;
}

#define HOOKS_SIZE(hooks) \
    (sizeof(hooks) / sizeof(hooks[0]))


int strendswith(const char *str, const char *suffix, int lensuf)
{
    unsigned int lenstr = strlen(str);
    return strcmp(str + lenstr - lensuf, suffix) == 0;
}

void* hybris_get_hooked_symbol(const char *sym, const char *requester)
{
    static int sorted = 0;
    static intptr_t counter = -1;
    static int do_print_unhooked = -1;
    void *found = NULL;
    struct _hook key;
    int sdk_version = -1;

    /* First check if we have a callback registered which could
     * give us a context specific hook implementation */
    if (hook_callback)
    {
        found = hook_callback(sym, requester);
        if (found)
            return (void*) found;
    }

#ifdef WANT_ADRENO_QUIRKS
    if (strendswith(requester, "libllvm-glnext.so", 17) && strcmp(sym, "malloc") == 0) {
        return _hybris_hook_malloc45;
    }
#endif

    if (!sorted)
    {
        qsort(hooks_properties, HOOKS_SIZE(hooks_properties), sizeof(hooks_properties[0]), hook_cmp);
        qsort(hooks_common, HOOKS_SIZE(hooks_common), sizeof(hooks_common[0]), hook_cmp);
        qsort(hooks_mm, HOOKS_SIZE(hooks_mm), sizeof(hooks_mm[0]), hook_cmp);
        qsort(hooks_n, HOOKS_SIZE(hooks_n), sizeof(hooks_n[0]), hook_cmp);
        qsort(hooks_p, HOOKS_SIZE(hooks_p), sizeof(hooks_p[0]), hook_cmp);
        sorted = 1;
    }

    /* Allow newer hooks to override those which are available for all versions */
    key.name = sym;
    sdk_version = hybris_get_android_sdk_version();

#if defined(WANT_LINKER_O) || defined(WANT_LINKER_Q)
    if (sdk_version > 27)
        found = bsearch(&key, hooks_p, HOOKS_SIZE(hooks_p), sizeof(hooks_p[0]), hook_cmp);
#endif
#if defined(WANT_LINKER_N) || defined(WANT_LINKER_O) || defined(WANT_LINKER_Q)
    if (!found && sdk_version > 23)
        found = bsearch(&key, hooks_n, HOOKS_SIZE(hooks_n), sizeof(hooks_n[0]), hook_cmp);
#endif
#if defined(WANT_LINKER_MM) || defined(WANT_LINKER_N) || defined(WANT_LINKER_O) || defined(WANT_LINKER_Q)
    if (!found && sdk_version > 21)
        found = bsearch(&key, hooks_mm, HOOKS_SIZE(hooks_mm), sizeof(hooks_mm[0]), hook_cmp);
#endif
    // make sure to skip the property hooks only when o.so is actually loaded
    // since for testing and we sometimes set things like 99 as sdk version.
    // The o linker is loaded when sdk_version >= 27 and exists.
    if (!found && sdk_version < 27)
        found = bsearch(&key, hooks_properties, HOOKS_SIZE(hooks_properties), sizeof(hooks_properties[0]), hook_cmp);

    if (!found)
        found = bsearch(&key, hooks_common, HOOKS_SIZE(hooks_common), sizeof(hooks_common[0]), hook_cmp);

    if (found)
    {
        if(hybris_should_trace(NULL, NULL))
            return ((struct _hook*) found)->debug_func;
        else
            return ((struct _hook*) found)->func;
    }

    if (strncmp(sym, "pthread", 7) == 0 ||
        strncmp(sym, "__pthread", 9) == 0)
    {
        /* safe */
        if (strcmp(sym, "pthread_sigmask") == 0)
           return NULL;
        /* not safe */
        counter--;
        // If you're experiencing a crash later on check the address of the
        // function pointer being call. If it matches the printed counter
        // value here then you can easily find out which symbol is missing.
        LOGD("Missing hook for pthread symbol %s (counter %" PRIiPTR ")\n", sym, counter);
        return (void *) counter;
    }

    if (do_print_unhooked == -1) {
        do_print_unhooked = !getenv("HYBRIS_DONT_PRINT_SYMBOLS_WITHOUT_HOOK");
    }

    if (do_print_unhooked) {
        LOGD("Could not find a hook for symbol %s", sym);
    }

    return NULL;
}
