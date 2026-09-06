#include <pthread.h>

// Destroy valid static initializers before any lock/wait operation, then
// explicitly reinitialize the same storage and exercise its normal lifecycle.
static int sync_destroy_lifecycle(unsigned kind) {
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
