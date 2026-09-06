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

#endif
