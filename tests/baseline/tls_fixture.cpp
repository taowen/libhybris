// Bionic-built DSO: compiler-generated thread_local initialization/destruction.
#include <pthread.h>
#include <stddef.h>
#include <time.h>
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
