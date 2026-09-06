#ifndef HYBRIS_BIONIC_SYNC_H
#define HYBRIS_BIONIC_SYNC_H

#include <pthread.h>
#include <stdint.h>

/* Base address to check for Android specifics */
#define ANDROID_TOP_ADDR_VALUE_MUTEX  0xFFFF
#define ANDROID_TOP_ADDR_VALUE_COND   0xFFFF
#define ANDROID_TOP_ADDR_VALUE_RWLOCK 0xFFFF

/* For the static initializer types */
#define ANDROID_PTHREAD_MUTEX_INITIALIZER            0
#define ANDROID_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define ANDROID_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000
#define ANDROID_PTHREAD_COND_INITIALIZER             0
#define ANDROID_PTHREAD_RWLOCK_INITIALIZER           0

/* Private common-library helpers; not a linker plugin or public ABI. */
__attribute__((visibility("hidden")))
uintptr_t hybris_read_sync_value(const void *storage);
__attribute__((visibility("hidden")))
pthread_mutex_t *hybris_get_static_mutex(void *storage);
__attribute__((visibility("hidden")))
pthread_cond_t *hybris_get_static_cond(void *storage);
__attribute__((visibility("hidden")))
uintptr_t hybris_get_static_rwlock_value(void *storage);

/* Private ABI hooks used by the central symbol registration table. */
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_init(pthread_mutex_t *__mutex,
                          __const pthread_mutexattr_t *__mutexattr);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_destroy(pthread_mutex_t *__mutex);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_lock(pthread_mutex_t *__mutex);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_trylock(pthread_mutex_t *__mutex);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_unlock(pthread_mutex_t *__mutex);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_lock_timeout_np(pthread_mutex_t *__mutex, unsigned __msecs);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutex_timedlock(pthread_mutex_t *__mutex,
                                      const struct timespec *__abs_timeout);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_mutexattr_setpshared(pthread_mutexattr_t *__attr,
                                           int pshared);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_init(pthread_cond_t *cond,
                                const pthread_condattr_t *attr);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_destroy(pthread_cond_t *cond);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_broadcast(pthread_cond_t *cond);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_signal(pthread_cond_t *cond);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_clockwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                 clockid_t clock_id, const struct timespec *abstime);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_timedwait_monotonic(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *abstime);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_timedwait(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *abstime);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                pthread_mutex_t *mutex, const struct timespec *reltime);

/* Rwlock ABI hooks; the two existing kind accessors retain public visibility. */
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlockattr_init(pthread_rwlockattr_t *__attr);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlockattr_destroy(pthread_rwlockattr_t *__attr);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlockattr_setpshared(pthread_rwlockattr_t *__attr,
                                            int pshared);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlockattr_getpshared(pthread_rwlockattr_t *__attr,
                                            int *pshared);
int _hybris_hook_pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *attr, int pref);
int _hybris_hook_pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *attr, int *pref);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_init(pthread_rwlock_t *__rwlock,
                                  __const pthread_rwlockattr_t *__attr);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_destroy(pthread_rwlock_t *__rwlock);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_rdlock(pthread_rwlock_t *__rwlock);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_tryrdlock(pthread_rwlock_t *__rwlock);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_timedrdlock(pthread_rwlock_t *__rwlock,
                                         __const struct timespec *abs_timeout);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_wrlock(pthread_rwlock_t *__rwlock);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_trywrlock(pthread_rwlock_t *__rwlock);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_timedwrlock(pthread_rwlock_t *__rwlock,
                                         __const struct timespec *abs_timeout);
__attribute__((visibility("hidden")))
int _hybris_hook_pthread_rwlock_unlock(pthread_rwlock_t *__rwlock);

#endif
