#include "probe.h"
#include <sched.h>

/* Exercise actual bionic pthread imports on fresh static mutexes in the DSO. */
struct mutex_race {
  unsigned active[32];
  pthread_barrier_t barrier;
  int start;
  int (*lock)(unsigned), (*unlock)(unsigned), (*destroy)(unsigned);
};
struct mutex_worker { struct mutex_race *race; int failed; };

static void *mutex_run(void *opaque) {
  struct mutex_worker *w = opaque;
  struct mutex_race *r = w->race;
  int start;
  while (!(start = __atomic_load_n(&r->start, __ATOMIC_ACQUIRE))) sched_yield();
  if (start < 0) return NULL;
  for (unsigned i = 0; i < 32; ++i) {
    pthread_barrier_wait(&r->barrier);
    if (r->lock(i)) {
      w->failed = 1;
      continue;
    }
    if (__atomic_add_fetch(&r->active[i], 1, __ATOMIC_SEQ_CST) != 1)
      w->failed = 1;
    /* Give another incorrectly published backing lock time to enter. */
    usleep(100);
    __atomic_sub_fetch(&r->active[i], 1, __ATOMIC_SEQ_CST);
    if (r->unlock(i)) w->failed = 1;
  }
  return NULL;
}

int mutex_init_probe(void) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*sym_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  if (!open_android || !sym_android || !close_android) return 2;
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) { printf("mutex fixture load failed\n"); return 2; }
  struct mutex_race r = {0};
  r.lock = sym_android(fixture, "mutex_fixture_lock");
  r.unlock = sym_android(fixture, "mutex_fixture_unlock");
  r.destroy = sym_android(fixture, "mutex_fixture_destroy");
  if (!r.lock || !r.unlock || !r.destroy || pthread_barrier_init(&r.barrier, NULL, 4))
    return 2;
  struct mutex_worker workers[4] = {0};
  pthread_t threads[4];
  unsigned started = 0;
  for (; started < 4; ++started) {
    workers[started].race = &r;
    if (pthread_create(&threads[started], NULL, mutex_run, &workers[started])) break;
  }
  __atomic_store_n(&r.start, started == 4 ? 1 : -1, __ATOMIC_RELEASE);
  int rc = started == 4 ? 0 : 2;
  for (unsigned i = 0; i < started; ++i) {
    if (pthread_join(threads[i], NULL)) return 2;
    printf("MUTEX_INIT worker=%u failed=%d\n", i, workers[i].failed);
    if (workers[i].failed) rc = 2;
  }
  if (started == 4)
    for (unsigned i = 0; i < 32; ++i)
      if (r.destroy(i)) rc = 2;
  pthread_barrier_destroy(&r.barrier);
  if (close_android(fixture)) rc = 2;
  printf("MUTEX_INIT %s (32 fresh locks, four concurrent first users)\n", rc ? "FAIL" : "PASS");
  return rc;
}
