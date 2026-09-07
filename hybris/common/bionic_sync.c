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
#include "bionic_sync.h"
#include "hooks_shm.h"
#include "logging.h"
#include <hybris/common/binding.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Owns static synchronization-object publication and synchronization ABI hooks.
 * The registration table in hooks.c selects these private entry points. */
static void hybris_set_mutex_attr(unsigned int android_value, pthread_mutexattr_t *attr)
{
    /* Init already sets as PTHREAD_MUTEX_NORMAL */
    pthread_mutexattr_init(attr);

    if (android_value & ANDROID_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) {
        pthread_mutexattr_settype(attr, PTHREAD_MUTEX_RECURSIVE);
    } else if (android_value & ANDROID_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) {
        pthread_mutexattr_settype(attr, PTHREAD_MUTEX_ERRORCHECK);
    }
}

/* Android mutex storage is only four-byte aligned, even on AArch64. Use
 * memcpy under a host lock: pointer-width atomics can SIGBUS on valid bionic
 * objects. This lock protects publication only, never the user's critical
 * section or a wait on its backing mutex. */
static pthread_mutex_t static_sync_guard = PTHREAD_MUTEX_INITIALIZER;

uintptr_t hybris_read_sync_value(const void *storage)
{
    uintptr_t value;
    pthread_mutex_lock(&static_sync_guard);
    memcpy(&value, storage, sizeof(value));
    pthread_mutex_unlock(&static_sync_guard);
    return value;
}

pthread_mutex_t* hybris_get_static_mutex(void *storage)
{
    uintptr_t value;
    pthread_mutex_lock(&static_sync_guard);
    memcpy(&value, storage, sizeof(value));
    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        pthread_mutex_t *candidate = malloc(sizeof(*candidate));
        pthread_mutexattr_t attr;
        if (!candidate) {
            fprintf(stderr, "HYBRIS: fatal: cannot allocate static mutex\n");
            abort();
        }
        hybris_set_mutex_attr(value, &attr);
        int error = pthread_mutex_init(candidate, &attr);
        pthread_mutexattr_destroy(&attr);
        if (error) {
            fprintf(stderr, "HYBRIS: fatal: cannot initialize static mutex (%d)\n", error);
            abort();
        }
        value = (uintptr_t)candidate;
        memcpy(storage, &value, sizeof(value));
    }
    pthread_mutex_unlock(&static_sync_guard);
    return (pthread_mutex_t *)value;
}

pthread_cond_t* hybris_get_static_cond(void *storage)
{
    uintptr_t value;
    pthread_mutex_lock(&static_sync_guard);
    memcpy(&value, storage, sizeof(value));
    if (value <= ANDROID_TOP_ADDR_VALUE_COND) {
        pthread_cond_t *candidate = malloc(sizeof(*candidate));
        if (!candidate) {
            fprintf(stderr, "HYBRIS: fatal: cannot allocate static condition variable\n");
            abort();
        }
        int error = pthread_cond_init(candidate, NULL);
        if (error) {
            fprintf(stderr, "HYBRIS: fatal: cannot initialize static condition variable (%d)\n", error);
            abort();
        }
        value = (uintptr_t)candidate;
        memcpy(storage, &value, sizeof(value));
    }
    pthread_mutex_unlock(&static_sync_guard);
    return (pthread_cond_t *)value;
}

static pthread_rwlock_t* hybris_alloc_init_rwlock(void)
{
    pthread_rwlock_t *realrwlock = malloc(sizeof(*realrwlock));
    if (!realrwlock) {
        fprintf(stderr, "HYBRIS: fatal: cannot allocate static rwlock\n");
        abort();
    }
    int error = pthread_rwlock_init(realrwlock, NULL);
    if (error) {
        fprintf(stderr, "HYBRIS: fatal: cannot initialize static rwlock (%d)\n", error);
        abort();
    }
    return realrwlock;
}

/* Return the stored value unchanged for an existing shared-memory handle.
 * API hooks translate handles after releasing the publication guard. */
uintptr_t hybris_get_static_rwlock_value(void *storage)
{
    uintptr_t value;
    pthread_mutex_lock(&static_sync_guard);
    memcpy(&value, storage, sizeof(value));
    if (value <= ANDROID_TOP_ADDR_VALUE_RWLOCK) {
        value = (uintptr_t)hybris_alloc_init_rwlock();
        memcpy(storage, &value, sizeof(value));
    }
    pthread_mutex_unlock(&static_sync_guard);
    return value;
}

#define LOGD(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)
#define TRACE_HOOK(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

/*
 * pthread_mutex* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_mutex_t struct differences between Bionic and Glibc.
 *
 * */

int _hybris_hook_pthread_mutex_init(pthread_mutex_t *__mutex,
                          __const pthread_mutexattr_t *__mutexattr)
{
    pthread_mutex_t *realmutex = NULL;

    TRACE_HOOK("mutex %p attr %p", __mutex, __mutexattr);

    int pshared = 0;
    if (__mutexattr)
        pthread_mutexattr_getpshared(__mutexattr, &pshared);

    if (!pshared) {
        /* non shared, standard mutex: use malloc */
        realmutex = malloc(sizeof(pthread_mutex_t));

        *((uintptr_t *)__mutex) = (uintptr_t) realmutex;
    }
    else {
        /* process-shared mutex: use the shared memory segment */
        hybris_shm_pointer_t handle = hybris_shm_alloc(sizeof(pthread_mutex_t));

        *((hybris_shm_pointer_t *)__mutex) = handle;

        if (handle)
            realmutex = (pthread_mutex_t *)hybris_get_shmpointer(handle);
    }

    if (!realmutex)
        return ENOMEM;
    return pthread_mutex_init(realmutex, __mutexattr);
}

int _hybris_hook_pthread_mutex_destroy(pthread_mutex_t *__mutex)
{
    int ret;

    TRACE_HOOK("mutex %p", __mutex);

    if (!__mutex)
        return EINVAL;

    uintptr_t value = hybris_read_sync_value(__mutex);
    /* A valid, unused static initializer owns no host allocation. */
    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX &&
        !hybris_check_android_shared_mutex(value)) {
        value = 0;
        memcpy(__mutex, &value, sizeof(value));
        return 0;
    }
    pthread_mutex_t *realmutex = (pthread_mutex_t *)value;

    if (!realmutex)
        return EINVAL;

    if (!hybris_is_pointer_in_shm((void*)realmutex)) {
        ret = pthread_mutex_destroy(realmutex);
        if (!ret)
            free(realmutex);
    }
    else {
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)realmutex);
        if (!realmutex)
            return EINVAL;
        ret = pthread_mutex_destroy(realmutex);
    }

    /* Failed destruction (notably EBUSY) leaves a usable owned mutex. */
    if (!ret) {
        value = 0;
        memcpy(__mutex, &value, sizeof(value));
    }

    return ret;
}

int _hybris_hook_pthread_mutex_lock(pthread_mutex_t *__mutex)
{
    TRACE_HOOK("mutex %p", __mutex);

    if (!__mutex) {
        LOGD("Null mutex lock, not locking.");
        return 0;
    }

    uintptr_t value = hybris_read_sync_value(__mutex);
    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not locking.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        TRACE("value %p <= ANDROID_TOP_ADDR_VALUE_MUTEX 0x%x",
              (void*) value, ANDROID_TOP_ADDR_VALUE_MUTEX);
        realmutex = hybris_get_static_mutex(__mutex);
    }

    return pthread_mutex_lock(realmutex);
}

int _hybris_hook_pthread_mutex_trylock(pthread_mutex_t *__mutex)
{
    uintptr_t value = hybris_read_sync_value(__mutex);

    TRACE_HOOK("mutex %p", __mutex);

    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not try locking.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_get_static_mutex(__mutex);
    }

    return pthread_mutex_trylock(realmutex);
}

int _hybris_hook_pthread_mutex_unlock(pthread_mutex_t *__mutex)
{
    TRACE_HOOK("mutex %p", __mutex);

    if (!__mutex) {
        LOGD("Null mutex lock, not unlocking.");
        return 0;
    }

    uintptr_t value = hybris_read_sync_value(__mutex);
    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not unlocking.");
        return 0;
    }

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        LOGD("Trying to unlock a lock that's not locked/initialized"
               " by Hybris, not unlocking.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    return pthread_mutex_unlock(realmutex);
}

int _hybris_hook_pthread_mutex_lock_timeout_np(pthread_mutex_t *__mutex, unsigned __msecs)
{
    struct timespec tv;
    pthread_mutex_t *realmutex;
    uintptr_t value = hybris_read_sync_value(__mutex);

    TRACE_HOOK("mutex %p msecs %u", __mutex, __msecs);

    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not lock timeout np.");
        return 0;
    }

    realmutex = (pthread_mutex_t *) value;

    if (hybris_is_pointer_in_shm((void *)value)) {
        realmutex = hybris_get_shmpointer((hybris_shm_pointer_t)value);
        if (!realmutex)
            return EINVAL;
    }

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_get_static_mutex(__mutex);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &tv))
        return errno;
    tv.tv_sec += __msecs/1000;
    tv.tv_nsec += (__msecs % 1000) * 1000000;
    if (tv.tv_nsec >= 1000000000) {
      tv.tv_sec++;
      tv.tv_nsec -= 1000000000;
    }

    int error = pthread_mutex_clocklock(realmutex, CLOCK_MONOTONIC, &tv);
    return error == ETIMEDOUT ? EBUSY : error;
}

static int mutex_timedlock_clock(pthread_mutex_t *__mutex,
                                 const struct timespec *__abs_timeout, clockid_t clock)
{
    TRACE_HOOK("mutex %p abs timeout %p", __mutex, __abs_timeout);

    if (!__mutex) {
        LOGD("Null mutex lock, not unlocking.");
        return 0;
    }

    uintptr_t value = hybris_read_sync_value(__mutex);
    if (hybris_check_android_shared_mutex(value)) {
        LOGD("Shared mutex with Android, not lock timeout np.");
        return 0;
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) value;
    if (hybris_is_pointer_in_shm((void *)value)) {
        realmutex = hybris_get_shmpointer((hybris_shm_pointer_t)value);
        if (!realmutex)
            return EINVAL;
    }

    if (value <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_get_static_mutex(__mutex);
    }

    if (!__abs_timeout)
        return pthread_mutex_lock(realmutex);
    return pthread_mutex_clocklock(realmutex, clock, __abs_timeout);
}

int _hybris_hook_pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline)
{
    return mutex_timedlock_clock(mutex, deadline, CLOCK_REALTIME);
}

int _hybris_hook_pthread_mutex_timedlock_monotonic_np(pthread_mutex_t *mutex,
                                                   const struct timespec *deadline)
{
    return mutex_timedlock_clock(mutex, deadline, CLOCK_MONOTONIC);
}

int _hybris_hook_pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr,
                                           int pshared)
{
    TRACE_HOOK("attr %p pshared %d", __attr, pshared);

    return pthread_mutexattr_setpshared(__attr, pshared);
}

/*
 * pthread_rwlockattr_* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_rwlockattr_t struct differences between Bionic and Glibc.
 *
 * */

int _hybris_hook_pthread_rwlockattr_init(pthread_rwlockattr_t *__attr)
{
    pthread_rwlockattr_t *realattr;

    TRACE_HOOK("attr %p", __attr);

    realattr = malloc(sizeof(pthread_rwlockattr_t));
    *((uintptr_t *)__attr) = (uintptr_t) realattr;

    return pthread_rwlockattr_init(realattr);
}

int _hybris_hook_pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr)
{
    int ret;
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p", __attr);

    ret = pthread_rwlockattr_destroy(realattr);
    free(realattr);

    return ret;
}

int _hybris_hook_pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr,
                                            int pshared)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p pshared %d", __attr, pshared);

    return pthread_rwlockattr_setpshared(realattr, pshared);
}

int _hybris_hook_pthread_rwlockattr_getpshared(pthread_rwlockattr_t *__attr,
                                            int *pshared)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    TRACE_HOOK("attr %p pshared %p", __attr, pshared);

    return pthread_rwlockattr_getpshared(realattr, pshared);
}

int _hybris_hook_pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *attr, int pref)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) attr;

    TRACE_HOOK("attr %p pref %i", attr, pref);

    /* Bionic has two values (0, 1); glibc inserts PREFER_WRITER_NP at 1. */
    switch (pref) {
    case 0:
        return pthread_rwlockattr_setkind_np(realattr, PTHREAD_RWLOCK_PREFER_READER_NP);
    case 1:
        return pthread_rwlockattr_setkind_np(realattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    default:
        return EINVAL;
    }
}

int _hybris_hook_pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *attr, int *pref)
{
    pthread_rwlockattr_t *realattr = (pthread_rwlockattr_t *) *(uintptr_t *) attr;

    TRACE_HOOK("attr %p pref %p", attr, pref);

    int host_pref;
    int error = pthread_rwlockattr_getkind_np(realattr, &host_pref);
    if (error)
        return error;
    switch (host_pref) {
    case PTHREAD_RWLOCK_PREFER_READER_NP:
        *pref = 0;
        return 0;
    case PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP:
        *pref = 1;
        return 0;
    default:
        /* A host-only policy has no bionic representation. */
        return EINVAL;
    }
}

/*
 * pthread_rwlock_* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_rwlock_t struct differences between Bionic and Glibc.
 *
 * */

int _hybris_hook_pthread_rwlock_init(pthread_rwlock_t *__rwlock,
                                  __const pthread_rwlockattr_t *__attr)
{
    pthread_rwlock_t *realrwlock = NULL;
    pthread_rwlockattr_t *realattr = NULL;
    int pshared = 0;

    TRACE_HOOK("rwlock %p attr %p", __rwlock, __attr);

    if (__attr != NULL)
        realattr = (pthread_rwlockattr_t *) *(uintptr_t *) __attr;

    if (realattr)
        pthread_rwlockattr_getpshared(realattr, &pshared);

    if (!pshared) {
        /* non shared, standard rwlock: use malloc */
        realrwlock = malloc(sizeof(pthread_rwlock_t));

        *((uintptr_t *) __rwlock) = (uintptr_t) realrwlock;
    }
    else {
        /* process-shared condition: use the shared memory segment */
        hybris_shm_pointer_t handle = hybris_shm_alloc(sizeof(pthread_rwlock_t));

        *((uintptr_t *)__rwlock) = (uintptr_t) handle;

        if (handle)
            realrwlock = (pthread_rwlock_t *)hybris_get_shmpointer(handle);
    }

    if (!realrwlock)
        return ENOMEM;
    return pthread_rwlock_init(realrwlock, realattr);
}

int _hybris_hook_pthread_rwlock_destroy(pthread_rwlock_t *__rwlock)
{
    int ret;
    uintptr_t value = hybris_read_sync_value(__rwlock);
    /* An unused static initializer has no host rwlock to destroy. */
    if (value <= ANDROID_TOP_ADDR_VALUE_RWLOCK) {
        value = 0;
        memcpy(__rwlock, &value, sizeof(value));
        return 0;
    }
    pthread_rwlock_t *realrwlock = (pthread_rwlock_t *)value;

    TRACE_HOOK("rwlock %p", __rwlock);

    if (!hybris_is_pointer_in_shm((void*)realrwlock)) {
        ret = pthread_rwlock_destroy(realrwlock);
        free(realrwlock);
    }
    else {
        realrwlock = hybris_get_shmpointer((hybris_shm_pointer_t)value);
        if (!realrwlock)
            return EINVAL;
        ret = pthread_rwlock_destroy(realrwlock);
    }

    return ret;
}

static pthread_rwlock_t* hybris_set_realrwlock(pthread_rwlock_t *rwlock)
{
    uintptr_t value = hybris_get_static_rwlock_value(rwlock);

    if (hybris_is_pointer_in_shm((void*)value))
        return (pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);
    return (pthread_rwlock_t *)value;
}

int _hybris_hook_pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_rdlock(realrwlock);
}

int _hybris_hook_pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_tryrdlock(realrwlock);
}

int _hybris_hook_pthread_rwlock_timedrdlock(pthread_rwlock_t *__rwlock,
                                         __const struct timespec *abs_timeout)
{
    TRACE_HOOK("rwlock %p abs timeout %p", __rwlock, abs_timeout);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_timedrdlock(realrwlock, abs_timeout);
}

int _hybris_hook_pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_wrlock(realrwlock);
}

int _hybris_hook_pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock)
{
    TRACE_HOOK("rwlock %p", __rwlock);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_trywrlock(realrwlock);
}

int _hybris_hook_pthread_rwlock_timedwrlock(pthread_rwlock_t *__rwlock,
                                         __const struct timespec *abs_timeout)
{
    TRACE_HOOK("rwlock %p abs timeout %p", __rwlock, abs_timeout);

    pthread_rwlock_t *realrwlock = hybris_set_realrwlock(__rwlock);

    return pthread_rwlock_timedwrlock(realrwlock, abs_timeout);
}

int _hybris_hook_pthread_rwlock_timedrdlock_monotonic_np(pthread_rwlock_t *rwlock,
                                                       const struct timespec *deadline)
{
    pthread_rwlock_t *real = hybris_set_realrwlock(rwlock);
    if (!real) return EINVAL;
    return deadline ? pthread_rwlock_clockrdlock(real, CLOCK_MONOTONIC, deadline)
                    : pthread_rwlock_rdlock(real);
}

int _hybris_hook_pthread_rwlock_timedwrlock_monotonic_np(pthread_rwlock_t *rwlock,
                                                       const struct timespec *deadline)
{
    pthread_rwlock_t *real = hybris_set_realrwlock(rwlock);
    if (!real) return EINVAL;
    return deadline ? pthread_rwlock_clockwrlock(real, CLOCK_MONOTONIC, deadline)
                    : pthread_rwlock_wrlock(real);
}

int _hybris_hook_pthread_rwlock_unlock(pthread_rwlock_t *__rwlock)
{
    uintptr_t value = hybris_read_sync_value(__rwlock);

    TRACE_HOOK("rwlock %p", __rwlock);

    if (value <= ANDROID_TOP_ADDR_VALUE_RWLOCK) {
        LOGD("Trying to unlock a rwlock that's not locked/initialized"
               " by Hybris, not unlocking.");
        return 0;
    }

    pthread_rwlock_t *realrwlock = (pthread_rwlock_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realrwlock = (pthread_rwlock_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    return pthread_rwlock_unlock(realrwlock);
}
