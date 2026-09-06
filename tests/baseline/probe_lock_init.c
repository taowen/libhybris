#include "probe.h"
#include <sched.h>
#include <errno.h>
#ifdef __BIONIC__
#include "sync_fixture.h"
#endif

/* Exercise actual bionic pthread imports on fresh static mutexes/rwlocks. */
struct lock_race {
  unsigned active[32];
  pthread_barrier_t barrier;
  int start;
  int (*lock)(unsigned), (*unlock)(unsigned), (*destroy)(unsigned);
  int (*read)(unsigned), (*trywrite)(unsigned);
};
struct lock_worker { struct lock_race *race; int failed; unsigned index; };

static void *lock_run(void *opaque) {
  struct lock_worker *w = opaque;
  struct lock_race *r = w->race;
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
  if (r->read) {
    for (unsigned i = 0; i < 32; ++i) {
      int acquired = r->read(i) == 0;
      if (!acquired) w->failed = 1;
      /* All four readers must hold the same lock before any reader releases. */
      pthread_barrier_wait(&r->barrier);
      if (w->index == 0) {
        int result = r->trywrite(i);
        if (result != EBUSY) {
          w->failed = 1;
          if (result == 0) r->unlock(i);
        }
      }
      pthread_barrier_wait(&r->barrier);
      if (acquired && r->unlock(i)) w->failed = 1;
    }
  }
  return NULL;
}

int lock_init_probe(int rwlock) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*sym_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  if (!open_android || !sym_android || !close_android) return 2;
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) { printf("mutex fixture load failed\n"); return 2; }
  struct lock_race r = {0};
  r.lock = sym_android(fixture, rwlock ? "rwlock_fixture_write" : "mutex_fixture_lock");
  r.unlock = sym_android(fixture, rwlock ? "rwlock_fixture_unlock" : "mutex_fixture_unlock");
  r.destroy = sym_android(fixture, rwlock ? "rwlock_fixture_destroy" : "mutex_fixture_destroy");
  if (rwlock) {
    r.read = sym_android(fixture, "rwlock_fixture_read");
    r.trywrite = sym_android(fixture, "rwlock_fixture_trywrite");
    if (!r.read || !r.trywrite) return 2;
  }
  const char *label = rwlock ? "RWLOCK_INIT" : "MUTEX_INIT";
  if (!r.lock || !r.unlock || !r.destroy || pthread_barrier_init(&r.barrier, NULL, 4))
    return 2;
  struct lock_worker workers[4] = {0};
  pthread_t threads[4];
  unsigned started = 0;
  for (; started < 4; ++started) {
    workers[started].race = &r;
    workers[started].index = started;
    if (pthread_create(&threads[started], NULL, lock_run, &workers[started])) break;
  }
  __atomic_store_n(&r.start, started == 4 ? 1 : -1, __ATOMIC_RELEASE);
  int rc = started == 4 ? 0 : 2;
  for (unsigned i = 0; i < started; ++i) {
    if (pthread_join(threads[i], NULL)) return 2;
    printf("%s worker=%u failed=%d\n", label, i, workers[i].failed);
    if (workers[i].failed) rc = 2;
  }
  if (started == 4)
    for (unsigned i = 0; i < 32; ++i)
      if (r.destroy(i)) rc = 2;
  pthread_barrier_destroy(&r.barrier);
  if (close_android(fixture)) rc = 2;
  printf("%s %s (32 fresh locks, four concurrent first users)\n", label, rc ? "FAIL" : "PASS");
  return rc;
}

int sync_destroy_probe(int check_kind) {
#ifdef __BIONIC__
  int (*destroy)(unsigned) = sync_destroy_lifecycle;
#else
  void *(*open_fixture)(const char *, int) = dlopen;
  void *(*find_fixture)(void *, const char *) = dlsym;
  int (*close_fixture)(void *) = dlclose;
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  open_fixture = dlsym(common, "android_dlopen");
  find_fixture = dlsym(common, "android_dlsym");
  close_fixture = dlsym(common, "android_dlclose");
  if (!open_fixture || !find_fixture || !close_fixture) return 2;
  void *fixture = open_fixture("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) return 2;
  int (*destroy)(unsigned) = find_fixture(fixture, check_kind ? "sync_fixture_kind" : "sync_fixture_destroy");
  if (!destroy) return 2;
#endif
  for (unsigned kind = 0; kind < (check_kind ? 1u : 5u); ++kind) {
    printf("SYNC_DESTROY begin kind=%u\n", kind);
    int error;
#ifdef __BIONIC__
    error = check_kind ? sync_kind_lifecycle() : destroy(kind);
#else
    error = destroy(kind);
#endif
    printf("SYNC_DESTROY kind=%u result=%d\n", kind, error);
    if (error) return 2;
  }
#ifndef __BIONIC__
  if (close_fixture(fixture)) return 2;
#endif
  printf("%s PASS\n", check_kind ? "SYNC_KIND" : "SYNC_DESTROY");
  return 0;
}
