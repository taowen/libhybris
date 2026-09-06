// Bionic-built DSO: compiler-generated thread_local initialization/destruction.
#include <pthread.h>
#include <stddef.h>
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
