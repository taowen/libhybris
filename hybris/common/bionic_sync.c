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

#define ANDROID_MUTEX_SHARED_MASK      0x2000
#define ANDROID_COND_SHARED_MASK       0x0001
#define ANDROID_COND_COUNTER_INCREMENT 0x0002
#define ANDROID_COND_COUNTER_MASK      (~ANDROID_COND_SHARED_MASK)

/* pthread cond struct as done in Android */
typedef struct {
    int volatile value;
} android_cond_t;

/* Helpers */
static int hybris_check_android_shared_mutex(uintptr_t mutex_addr)
{
    /* If not initialized or initialized by Android, it should contain a low
     * address, which is basically just the int values for Android's own
     * pthread_mutex_t */
    if ((mutex_addr <= ANDROID_TOP_ADDR_VALUE_MUTEX) &&
                    (mutex_addr & ANDROID_MUTEX_SHARED_MASK))
        return 1;

    return 0;
}

static int hybris_check_android_shared_cond(uintptr_t cond_addr)
{
    /* If not initialized or initialized by Android, it should contain a low
     * address, which is basically just the int values for Android's own
     * pthread_cond_t */
    if ((cond_addr <= ANDROID_TOP_ADDR_VALUE_COND) &&
                    (cond_addr & ANDROID_COND_SHARED_MASK))
        return 1;

    /* In case android is setting up cond_addr with a negative value,
     * used for error control */
    if (cond_addr > HYBRIS_SHM_MASK_TOP)
        return 1;

    return 0;
}

/* Based on Android's Bionic pthread implementation.
 * This is just needed when we have a shared cond with Android */
static int __android_pthread_cond_pulse(android_cond_t *cond, int counter)
{
    long flags;
    int fret;

    if (cond == NULL)
        return EINVAL;

    flags = (cond->value & ~ANDROID_COND_COUNTER_MASK);
    for (;;) {
        long oldval = cond->value;
        long newval = 0;
        /* In our case all we need to do is make sure the negative value
         * is under our range, which is the last 0xF from SHM_MASK */
        if (oldval < -12)
            newval = ((oldval + ANDROID_COND_COUNTER_INCREMENT) &
                            ANDROID_COND_COUNTER_MASK) | flags;
        else
            newval = ((oldval - ANDROID_COND_COUNTER_INCREMENT) &
                            ANDROID_COND_COUNTER_MASK) | flags;
        if (__sync_bool_compare_and_swap(&cond->value, oldval, newval))
            break;
    }

    int pshared = cond->value & ANDROID_COND_SHARED_MASK;
    fret = syscall(SYS_futex , &cond->value,
                   pshared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE, counter,
                   NULL, NULL, NULL);
    (void)fret;
    LOGD("futex based pthread_cond_*, value %d, counter %d, ret %d",
                                            cond->value, counter, fret);
    return 0;
}

int android_pthread_cond_broadcast(android_cond_t *cond)
{
    return __android_pthread_cond_pulse(cond, INT_MAX);
}

int android_pthread_cond_signal(android_cond_t *cond)
{
    return __android_pthread_cond_pulse(cond, 1);
}


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

    clock_gettime(CLOCK_REALTIME, &tv);
    tv.tv_sec += __msecs/1000;
    tv.tv_nsec += (__msecs % 1000) * 1000000;
    if (tv.tv_nsec >= 1000000000) {
      tv.tv_sec++;
      tv.tv_nsec -= 1000000000;
    }

    return pthread_mutex_timedlock(realmutex, &tv);
}

int _hybris_hook_pthread_mutex_timedlock(pthread_mutex_t *__mutex,
                                      const struct timespec *__abs_timeout)
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

    return pthread_mutex_timedlock(realmutex, __abs_timeout);
}

int _hybris_hook_pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr,
                                           int pshared)
{
    TRACE_HOOK("attr %p pshared %d", __attr, pshared);

    return pthread_mutexattr_setpshared(__attr, pshared);
}

/*
 * pthread_cond* functions
 *
 * Specific implementations to workaround the differences between at the
 * pthread_cond_t struct differences between Bionic and Glibc.
 *
 * */

int _hybris_hook_pthread_cond_init(pthread_cond_t *cond,
                                const pthread_condattr_t *attr)
{
    pthread_cond_t *realcond = NULL;

    TRACE_HOOK("cond %p attr %p", cond, attr);

    int pshared = 0;

    if (attr)
        pthread_condattr_getpshared(attr, &pshared);

    if (!pshared) {
        /* non shared, standard cond: use malloc */
        realcond = malloc(sizeof(pthread_cond_t));

        *((uintptr_t *) cond) = (uintptr_t) realcond;
    }
    else {
        /* process-shared condition: use the shared memory segment */
        hybris_shm_pointer_t handle = hybris_shm_alloc(sizeof(pthread_cond_t));

        *((uintptr_t *)cond) = (uintptr_t) handle;

        if (handle)
            realcond = (pthread_cond_t *)hybris_get_shmpointer(handle);
    }

    if (!realcond)
        return ENOMEM;
    return pthread_cond_init(realcond, attr);
}

int _hybris_hook_pthread_cond_destroy(pthread_cond_t *cond)
{
    int ret;
    uintptr_t value = hybris_read_sync_value(cond);
    /* No wait/signal has materialized this private static condition yet. */
    if (value <= ANDROID_TOP_ADDR_VALUE_COND &&
        !hybris_check_android_shared_cond(value)) {
        value = 0;
        memcpy(cond, &value, sizeof(value));
        return 0;
    }
    pthread_cond_t *realcond = (pthread_cond_t *)value;

    TRACE_HOOK("cond %p", cond);

    if (!realcond) {
      return EINVAL;
    }

    if (!hybris_is_pointer_in_shm((void*)realcond)) {
        /* Bionic and glibc implementations of pthread_cond_destroy are different.
         * Bionic implementation does not block whereas the glibc implementation
         * requires that there are no threads waiting for the condition variable
         * when it is destroyed and bionic code does not always follow this
         * requirement. To prevent deadlocks reset the reference count of the
         * condition variable. */
        realcond->__data.__wrefs = 0;
        ret = pthread_cond_destroy(realcond);
        free(realcond);
    }
    else {
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)realcond);
        ret = pthread_cond_destroy(realcond);
    }

    *((uintptr_t *)cond) = 0;

    return ret;
}

int _hybris_hook_pthread_cond_broadcast(pthread_cond_t *cond)
{
    uintptr_t value = hybris_read_sync_value(cond);

    TRACE_HOOK("cond %p", cond);

    if (hybris_check_android_shared_cond(value)) {
        LOGD("Shared condition with Android, broadcasting with futex.");
        return android_pthread_cond_broadcast((android_cond_t *) cond);
    }

    pthread_cond_t *realcond = (pthread_cond_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_get_static_cond(cond);
    }

    return pthread_cond_broadcast(realcond);
}

int _hybris_hook_pthread_cond_signal(pthread_cond_t *cond)
{
    uintptr_t value = hybris_read_sync_value(cond);

    TRACE_HOOK("cond %p", cond);

    if (hybris_check_android_shared_cond(value)) {
        LOGD("Shared condition with Android, broadcasting with futex.");
        return android_pthread_cond_signal((android_cond_t *) cond);
    }

    pthread_cond_t *realcond = (pthread_cond_t *) value;
    if (hybris_is_pointer_in_shm((void*)value))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)value);

    if (value <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_get_static_cond(cond);
    }

    return pthread_cond_signal(realcond);
}

int _hybris_hook_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    /* Both cond and mutex can be statically initialized, check for both */
    uintptr_t cvalue = hybris_read_sync_value(cond);
    uintptr_t mvalue = hybris_read_sync_value(mutex);

    TRACE_HOOK("cond %p mutex %p", cond, mutex);

    if (hybris_check_android_shared_cond(cvalue) ||
        hybris_check_android_shared_mutex(mvalue)) {
        LOGD("Shared condition/mutex with Android, not waiting.");
        return 0;
    }

    pthread_cond_t *realcond = (pthread_cond_t *) cvalue;
    if (hybris_is_pointer_in_shm((void*)cvalue))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)cvalue);

    if (cvalue <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_get_static_cond(cond);
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) mvalue;
    if (hybris_is_pointer_in_shm((void*)mvalue))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)mvalue);

    if (mvalue <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_get_static_mutex(mutex);
    }

    return pthread_cond_wait(realcond, realmutex);
}

int _hybris_hook_pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                 clockid_t clock_id, const struct timespec *abstime)
{
    /* Both cond and mutex can be statically initialized, check for both */
    uintptr_t cvalue = hybris_read_sync_value(cond);
    uintptr_t mvalue = hybris_read_sync_value(mutex);

    TRACE_HOOK("cond %p mutex %p abstime %p", cond, mutex, abstime);

    if (hybris_check_android_shared_cond(cvalue) ||
        hybris_check_android_shared_mutex(mvalue)) {
        LOGD("Shared condition/mutex with Android, not waiting.");
        return 0;
    }

    pthread_cond_t *realcond = (pthread_cond_t *) cvalue;
    if (hybris_is_pointer_in_shm((void*)cvalue))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)cvalue);

    if (cvalue <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_get_static_cond(cond);
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) mvalue;
    if (hybris_is_pointer_in_shm((void*)mvalue))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)mvalue);

    if (mvalue <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_get_static_mutex(mutex);
    }

    return pthread_cond_clockwait(realcond, realmutex, clock_id, abstime);
}

int _hybris_hook_pthread_cond_timedwait_monotonic(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *abstime)
{
    /* These Android aliases always use an absolute monotonic deadline,
     * independent of the condition variable's default realtime clock. */
    return _hybris_hook_pthread_cond_clockwait(cond, mutex, CLOCK_MONOTONIC, abstime);
}

int _hybris_hook_pthread_cond_timedwait(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *abstime)
{
    /* Both cond and mutex can be statically initialized, check for both */
    uintptr_t cvalue = hybris_read_sync_value(cond);
    uintptr_t mvalue = hybris_read_sync_value(mutex);

    TRACE_HOOK("cond %p mutex %p abstime %p", cond, mutex, abstime);

    if (hybris_check_android_shared_cond(cvalue) ||
         hybris_check_android_shared_mutex(mvalue)) {
        LOGD("Shared condition/mutex with Android, not waiting.");
        return 0;
    }

    pthread_cond_t *realcond = (pthread_cond_t *) cvalue;
    if (hybris_is_pointer_in_shm((void*)cvalue))
        realcond = (pthread_cond_t *)hybris_get_shmpointer((hybris_shm_pointer_t)cvalue);

    if (cvalue <= ANDROID_TOP_ADDR_VALUE_COND) {
        realcond = hybris_get_static_cond(cond);
    }

    pthread_mutex_t *realmutex = (pthread_mutex_t *) mvalue;
    if (hybris_is_pointer_in_shm((void*)mvalue))
        realmutex = (pthread_mutex_t *)hybris_get_shmpointer((hybris_shm_pointer_t)mvalue);

    if (mvalue <= ANDROID_TOP_ADDR_VALUE_MUTEX) {
        realmutex = hybris_get_static_mutex(mutex);
    }

    return pthread_cond_timedwait(realcond, realmutex, abstime);
}

int _hybris_hook_pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *reltime)
{
    if (!reltime || reltime->tv_sec < 0 || reltime->tv_nsec < 0 ||
        reltime->tv_nsec >= 1000000000)
        return EINVAL;

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline))
        return errno;
    /* Reject unrepresentable deadlines without signed overflow. */
    if (__builtin_add_overflow(deadline.tv_sec, reltime->tv_sec, &deadline.tv_sec))
        return EINVAL;
    deadline.tv_nsec += reltime->tv_nsec;
    if (deadline.tv_nsec >= 1000000000) {
        if (__builtin_add_overflow(deadline.tv_sec, (time_t)1, &deadline.tv_sec))
            return EINVAL;
        deadline.tv_nsec -= 1000000000;
    }
    return _hybris_hook_pthread_cond_clockwait(cond, mutex, CLOCK_MONOTONIC, &deadline);
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
