#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

// Destroy valid static initializers before any lock/wait operation, then
// explicitly reinitialize the same storage and exercise its normal lifecycle.
static int sync_destroy_lifecycle(unsigned kind) {
    if (kind >= 5 && kind <= 7) {
        const int types[] = {PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_ERRORCHECK};
        pthread_mutexattr_t attr;
        pthread_mutex_t lock;
        int error = pthread_mutexattr_init(&attr);
        if (error) return error;
        error = pthread_mutexattr_settype(&attr, types[kind - 5]);
        if (!error) error = pthread_mutex_init(&lock, &attr);
        pthread_mutexattr_destroy(&attr);
        if (error) return error;
        error = pthread_mutex_lock(&lock);
        if (error) return error;
        unsigned char before[sizeof(lock)];
        memcpy(before, &lock, sizeof(lock));
        int result = pthread_mutex_destroy(&lock);
        int preserved = !memcmp(before, &lock, sizeof(lock));
        printf("SYNC_DESTROY_BUSY type=%d result=%d expected=%d preserved=%d\n",
               types[kind - 5], result, EBUSY, preserved);
        // Do not touch a discarded backing pointer after a failed check.
        if (result != EBUSY || !preserved) return EINVAL;
        error = pthread_mutex_unlock(&lock);
        if (!error) error = pthread_mutex_lock(&lock);
        if (!error) error = pthread_mutex_unlock(&lock);
        if (!error) error = pthread_mutex_destroy(&lock);
        return error;
    }
    if (kind < 3) {
        pthread_mutex_t normal = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_t recursive = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
        pthread_mutex_t errorcheck = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
        pthread_mutex_t *p = kind == 0 ? &normal : kind == 1 ? &recursive : &errorcheck;
        int error = pthread_mutex_destroy(p);
        if (!error) error = pthread_mutex_init(p, NULL);
        if (!error) error = pthread_mutex_lock(p);
        if (!error) error = pthread_mutex_unlock(p);
        if (!error) error = pthread_mutex_destroy(p);
        return error;
    }
    if (kind == 3) {
        pthread_cond_t c = PTHREAD_COND_INITIALIZER;
        int error = pthread_cond_destroy(&c);
        if (!error) error = pthread_cond_init(&c, NULL);
        if (!error) error = pthread_cond_signal(&c);
        if (!error) error = pthread_cond_destroy(&c);
        return error;
    }
    pthread_rwlock_t r = PTHREAD_RWLOCK_INITIALIZER;
    int error = pthread_rwlock_destroy(&r);
    if (!error) error = pthread_rwlock_init(&r, NULL);
    if (!error) error = pthread_rwlock_wrlock(&r);
    if (!error) error = pthread_rwlock_unlock(&r);
    if (!error) error = pthread_rwlock_destroy(&r);
    return error;
}


// Bionic exposes only reader preference (0) and nonrecursive writer (1).
static int sync_kind_lifecycle(void) {
    pthread_rwlockattr_t attr;
    int error = pthread_rwlockattr_init(&attr);
    if (error) return error;
    int kind = -1;
    error = pthread_rwlockattr_getkind_np(&attr, &kind);
    if (!error && kind != PTHREAD_RWLOCK_PREFER_READER_NP) error = EINVAL;
    for (int requested = 0; !error && requested <= 1; ++requested) {
        error = pthread_rwlockattr_setkind_np(&attr, requested);
        if (!error) error = pthread_rwlockattr_getkind_np(&attr, &kind);
        if (!error && kind != requested) error = EINVAL;
        if (!error) {
            pthread_rwlock_t lock;
            error = pthread_rwlock_init(&lock, &attr);
            if (!error) error = pthread_rwlock_rdlock(&lock);
            if (!error) error = pthread_rwlock_unlock(&lock);
            if (!error) error = pthread_rwlock_wrlock(&lock);
            if (!error) error = pthread_rwlock_unlock(&lock);
            if (!error) error = pthread_rwlock_destroy(&lock);
        }
    }
    const int invalid[] = {-1, 2, 3, 2147483647};
    for (unsigned i = 0; !error && i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        int result = pthread_rwlockattr_setkind_np(&attr, invalid[i]);
        printf("SYNC_KIND rejected=%d result=%d expected=%d\n", invalid[i], result, EINVAL);
        if (result != EINVAL) error = EINVAL;
        if (!error) error = pthread_rwlockattr_getkind_np(&attr, &kind);
        if (!error && kind != PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) error = EINVAL;
    }
    int cleanup = pthread_rwlockattr_destroy(&attr);
    return error ? error : cleanup;
}
