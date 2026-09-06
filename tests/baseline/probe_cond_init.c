#include "probe.h"
#include <sched.h>

struct cond_race {
  pthread_barrier_t barrier;
  int start, ready;
  int (*wait)(unsigned, void (*)(void *), void *);
  int (*pulse)(unsigned, int), (*release)(unsigned), (*destroy)(unsigned);
};
struct cond_worker { struct cond_race *race; unsigned index, failures; };

static void cond_ready(void *opaque) {
  struct cond_race *r = opaque;
  __atomic_store_n(&r->ready, 1, __ATOMIC_RELEASE);
}

static void *cond_run(void *opaque) {
  struct cond_worker *w = opaque;
  struct cond_race *r = w->race;
  int start;
  while (!(start = __atomic_load_n(&r->start, __ATOMIC_ACQUIRE))) sched_yield();
  if (start < 0) return NULL;
  for (unsigned i = 0; i < 32; ++i) {
    pthread_barrier_wait(&r->barrier);
    int error = w->index == 0 ? r->wait(i, cond_ready, r) : r->pulse(i, w->index == 2);
    if (error) {
      ++w->failures;
      printf("COND_INIT worker=%u round=%u error=%d\n", w->index, i, error);
      if (w->index == 0) cond_ready(r);
    }
    pthread_barrier_wait(&r->barrier);
  }
  return NULL;
}

int cond_init_probe(void) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*sym_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  if (!open_android || !sym_android || !close_android) return 2;
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) return 2;
  struct cond_race r = {0};
  r.wait = sym_android(fixture, "cond_fixture_wait");
  r.pulse = sym_android(fixture, "cond_fixture_pulse");
  r.release = sym_android(fixture, "cond_fixture_release");
  r.destroy = sym_android(fixture, "cond_fixture_destroy");
  if (!r.wait || !r.pulse || !r.release || !r.destroy || pthread_barrier_init(&r.barrier, NULL, 4))
    return 2;
  struct cond_worker workers[3] = {0};
  pthread_t threads[3];
  unsigned started = 0;
  for (; started < 3; ++started) {
    workers[started].race = &r;
    workers[started].index = started;
    if (pthread_create(&threads[started], NULL, cond_run, &workers[started])) break;
  }
  __atomic_store_n(&r.start, started == 3 ? 1 : -1, __ATOMIC_RELEASE);
  int rc = started == 3 ? 0 : 2;
  if (!rc) {
    for (unsigned i = 0; i < 32; ++i) {
      __atomic_store_n(&r.ready, 0, __ATOMIC_RELEASE);
      pthread_barrier_wait(&r.barrier);
      while (!__atomic_load_n(&r.ready, __ATOMIC_ACQUIRE)) sched_yield();
      /* The predicate changes under the same bionic mutex used by wait.
       * Initial signal/broadcast calls are permitted spurious wakeups. */
      if (r.release(i)) rc = 2;
      pthread_barrier_wait(&r.barrier);
    }
  }
  for (unsigned i = 0; i < started; ++i) {
    if (pthread_join(threads[i], NULL)) return 2;
    printf("COND_INIT worker=%u failures=%u\n", i, workers[i].failures);
    if (workers[i].failures) rc = 2;
  }
  if (started == 3)
    for (unsigned i = 0; i < 32; ++i)
      if (r.destroy(i)) rc = 2;
  pthread_barrier_destroy(&r.barrier);
  if (close_android(fixture)) rc = 2;
  printf("COND_INIT %s (32 wait/signal/broadcast first-use races)\n", rc ? "FAIL" : "PASS");
  return rc;
}
