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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LOGD(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)
#define TRACE_HOOK(message, ...) HYBRIS_DEBUG_LOG(HOOKS, message, ##__VA_ARGS__)

#define ANDROID_COND_SHARED_MASK       0x0001
#define ANDROID_COND_COUNTER_INCREMENT 0x0002
#define ANDROID_COND_COUNTER_MASK      (~ANDROID_COND_SHARED_MASK)

/* pthread cond struct as done in Android */
typedef struct {
    int volatile value;
} android_cond_t;

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

/* Condition-variable ABI hooks. Static object publication remains owned by
 * bionic_sync.c so conditions and their mutexes use the same publication lock. */
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
