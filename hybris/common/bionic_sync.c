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

#include "bionic_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Owns process-private static synchronization-object allocation/publication.
 * API hooks retain explicit init/destroy, shared-handle translation and waits. */
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
 * Handle translation belongs to hooks.c and happens after releasing the guard. */
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
