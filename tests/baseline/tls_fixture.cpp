// Bionic-built DSO: compiler-generated thread_local initialization/destruction.
#include <pthread.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

extern "C" int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
extern "C" int pthread_cond_timedwait_monotonic_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
extern "C" int pthread_cond_timedwait_monotonic(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

extern "C" int cond_fixture_timeout(unsigned variant, long long *elapsed) {
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    struct timespec start, end, deadline;
    clock_gettime(CLOCK_MONOTONIC, &start);
    deadline = start;
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    int error = pthread_cond_init(&cond, NULL);
    if (error) return error;
    error = pthread_mutex_lock(&mutex);
    if (error) return error;
    do {
        if (variant >= 2) {
            struct timespec relative = {0, 100000000};
            if (variant == 3) relative.tv_nsec = 1000000000;
            if (variant == 4) relative.tv_nsec = -1;
            if (variant == 5) relative.tv_sec = LONG_MAX;
            if (variant == 6) relative.tv_sec = -1;
            error = pthread_cond_timedwait_relative_np(&cond, &mutex, &relative);
        } else error = variant ? pthread_cond_timedwait_monotonic_np(&cond, &mutex, &deadline)
                        : pthread_cond_timedwait_monotonic(&cond, &mutex, &deadline);
    } while (!error); // Allow spurious wakeups, retaining the original deadline.
    clock_gettime(CLOCK_MONOTONIC, &end);
    *elapsed = (end.tv_sec-start.tv_sec)*1000000000LL + end.tv_nsec-start.tv_nsec;
    int unlock = pthread_mutex_unlock(&mutex);
    int destroy_cond = pthread_cond_destroy(&cond);
    int destroy_mutex = pthread_mutex_destroy(&mutex);
    return unlock ? unlock : destroy_cond ? destroy_cond : destroy_mutex ? destroy_mutex : error;
}
struct Local {
    int value = 73;
    void (*callback)(void *, int) = nullptr;
    void *opaque = nullptr;
    ~Local() { if (callback) callback(opaque, value); }
};
static thread_local Local local;
extern "C" int tls_fixture_touch(void (*callback)(void *, int), void *opaque, int value) {
    int initial = local.value;
    local.callback = callback;
    local.opaque = opaque;
    local.value = value;
    return initial;
}

// Also expose real bionic pthread imports for the static-mutex first-use probe.
// The prefix deliberately covers four-byte, but not eight-byte, alignment.
struct MutexSlot { unsigned prefix; pthread_mutex_t mutex; };
static_assert(offsetof(MutexSlot, mutex) == 4, "probe requires bionic four-byte mutex alignment");
static MutexSlot mutex_slots[32] = {};
extern "C" int mutex_fixture_lock(unsigned i) { return pthread_mutex_lock(&mutex_slots[i].mutex); }
extern "C" int mutex_fixture_unlock(unsigned i) { return pthread_mutex_unlock(&mutex_slots[i].mutex); }
extern "C" int mutex_fixture_destroy(unsigned i) { return pthread_mutex_destroy(&mutex_slots[i].mutex); }

struct RWLockSlot { unsigned prefix; pthread_rwlock_t lock; };
static RWLockSlot rwlock_slots[32] = {};
extern "C" int rwlock_fixture_write(unsigned i) { return pthread_rwlock_wrlock(&rwlock_slots[i].lock); }
extern "C" int rwlock_fixture_read(unsigned i) { return pthread_rwlock_rdlock(&rwlock_slots[i].lock); }
extern "C" int rwlock_fixture_trywrite(unsigned i) { return pthread_rwlock_trywrlock(&rwlock_slots[i].lock); }
extern "C" int rwlock_fixture_unlock(unsigned i) { return pthread_rwlock_unlock(&rwlock_slots[i].lock); }
extern "C" int rwlock_fixture_destroy(unsigned i) { return pthread_rwlock_destroy(&rwlock_slots[i].lock); }

struct CondSlot { unsigned prefix; pthread_cond_t cond; pthread_mutex_t mutex; int released; };
static_assert(offsetof(CondSlot, cond) == 4, "probe requires bionic four-byte cond alignment");
static CondSlot cond_slots[32] = {};
extern "C" int cond_fixture_wait(unsigned i, void (*ready)(void *), void *opaque) {
    CondSlot *s = &cond_slots[i];
    int error = pthread_mutex_lock(&s->mutex);
    if (error) return error;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 500000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    ready(opaque);
    while (!s->released && !error)
        error = pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
    int unlocked = pthread_mutex_unlock(&s->mutex);
    return error ? error : unlocked;
}
extern "C" int cond_fixture_pulse(unsigned i, int broadcast) {
    return broadcast ? pthread_cond_broadcast(&cond_slots[i].cond) : pthread_cond_signal(&cond_slots[i].cond);
}
extern "C" int cond_fixture_release(unsigned i) {
    CondSlot *s = &cond_slots[i];
    int error = pthread_mutex_lock(&s->mutex);
    if (error) return error;
    s->released = 1;
    error = pthread_mutex_unlock(&s->mutex);
    return error ? error : pthread_cond_broadcast(&s->cond);
}
extern "C" int cond_fixture_destroy(unsigned i) {
    int error = pthread_cond_destroy(&cond_slots[i].cond);
    return error ? error : pthread_mutex_destroy(&cond_slots[i].mutex);
}

extern "C" int shared_fixture_init(unsigned kind) {
    int error;
    if (kind == 0) {
        pthread_mutexattr_t attr;
        pthread_mutex_t object;
        if ((error = pthread_mutexattr_init(&attr))) return error;
        error = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (!error) {
            error = pthread_mutex_init(&object, &attr);
            if (!error) pthread_mutex_destroy(&object);
        }
        pthread_mutexattr_destroy(&attr);
    } else if (kind == 1) {
        pthread_condattr_t attr;
        pthread_cond_t object;
        if ((error = pthread_condattr_init(&attr))) return error;
        error = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (!error) {
            error = pthread_cond_init(&object, &attr);
            if (!error) pthread_cond_destroy(&object);
        }
        pthread_condattr_destroy(&attr);
    } else {
        pthread_rwlockattr_t attr;
        pthread_rwlock_t object;
        if ((error = pthread_rwlockattr_init(&attr))) return error;
        error = pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (!error) {
            error = pthread_rwlock_init(&object, &attr);
            if (!error) pthread_rwlock_destroy(&object);
        }
        pthread_rwlockattr_destroy(&attr);
    }
    return error;
}

#include "sync_fixture.h"
extern "C" int sync_fixture_destroy(unsigned kind) { return sync_destroy_lifecycle(kind); }
extern "C" int sync_fixture_kind(unsigned unused) { (void)unused; return sync_kind_lifecycle(); }

#include "stdio_fixture.h"
extern "C" int stdio_fixture_flush(unsigned memory) { return memory < 2 ? stdio_flush_lifecycle(memory) : stdio_position_lifecycle(memory - 2); }

extern "C" int pthread_mutex_lock_timeout_np(pthread_mutex_t *, unsigned);
extern "C" int mutex_fixture_timeout(long long *elapsed) {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    int error = pthread_mutex_lock_timeout_np(&mutex, 0);
    if (error) return error;
    timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int result = pthread_mutex_lock_timeout_np(&mutex, 100);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    *elapsed = (finish.tv_sec - start.tv_sec) * 1000000000LL + finish.tv_nsec - start.tv_nsec;
    error = pthread_mutex_unlock(&mutex);
    if (!error) error = pthread_mutex_lock_timeout_np(&mutex, 0);
    if (!error) error = pthread_mutex_unlock(&mutex);
    if (!error) error = pthread_mutex_destroy(&mutex);
    return error ? error : result;
}

extern "C" int pthread_mutex_timedlock_monotonic_np(pthread_mutex_t *, const struct timespec *);
extern "C" int mutex_fixture_monotonic(void) {
    return sync_mutex_monotonic(pthread_mutex_timedlock_monotonic_np);
}

extern "C" int pthread_rwlock_timedrdlock_monotonic_np(pthread_rwlock_t *, const struct timespec *);
extern "C" int pthread_rwlock_timedwrlock_monotonic_np(pthread_rwlock_t *, const struct timespec *);
extern "C" int rwlock_fixture_monotonic(void) {
    return sync_rw_monotonic(pthread_rwlock_timedrdlock_monotonic_np,
                             pthread_rwlock_timedwrlock_monotonic_np);
}
