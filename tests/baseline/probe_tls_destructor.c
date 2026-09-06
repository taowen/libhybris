#include "probe.h"

typedef int (*touch_fn)(void (*)(void *, int), void *, int);
struct destructor_work {
  touch_fn touch;
  pthread_barrier_t barrier;
  pthread_t worker;
  int initial, count, value, same_thread;
};

static void observe_destructor(void *opaque, int value) {
  struct destructor_work *work = opaque;
  work->count++;
  work->value = value;
  work->same_thread = pthread_equal(pthread_self(), work->worker);
}

static void *destructor_worker(void *opaque) {
  struct destructor_work *work = opaque;
  work->worker = pthread_self();
  work->initial = work->touch(observe_destructor, work, 1234);
  pthread_barrier_wait(&work->barrier);
  pthread_barrier_wait(&work->barrier);
  return NULL;
}

int tls_destructor_probe(void) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) { printf("common load failed: %s\n", dlerror()); return 2; }
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*sym_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  char *(*error_android)(void) = dlsym(common, "android_dlerror");
  if (!open_android || !sym_android || !close_android || !error_android) return 2;
  const char *fixtures[] = {"./libtls-fixture.so", "./libtls-native-fixture.so"};
  for (unsigned variant = 0; variant < 2; ++variant) {
    printf("TLS_DTOR fixture=%s\n", fixtures[variant]);
    for (int cycle = 0; cycle < 3; ++cycle) {
      void *fixture = open_android(fixtures[variant], RTLD_NOW);
      if (!fixture) { printf("TLS fixture load failed: %s\n", error_android()); return 2; }
      struct destructor_work work = {0};
      work.touch = (touch_fn)sym_android(fixture, "tls_fixture_touch");
      if (!work.touch || pthread_barrier_init(&work.barrier, NULL, 2)) return 2;
      pthread_t thread;
      if (pthread_create(&thread, NULL, destructor_worker, &work)) {
        pthread_barrier_destroy(&work.barrier); close_android(fixture); return 2;
      }
      pthread_barrier_wait(&work.barrier);
      int early = work.count, initial = work.initial;
      int closed = close_android(fixture);
      /* Only the pending TLS destructor may need to keep the fixture resident. */
      pthread_barrier_wait(&work.barrier);
      if (pthread_join(thread, NULL)) return 2;
      pthread_barrier_destroy(&work.barrier);
      printf("TLS_DTOR cycle=%d initial=%d early=%d count=%d value=%d thread=%d close=%d\n",
             cycle, initial, early, work.count, work.value, work.same_thread, closed);
      if (initial != 73 || early != 0 || work.count != 1 || work.value != 1234 ||
          !work.same_thread || closed) return 2;
    }
  }
  printf("TLS_DTOR PASS (compiler-generated bionic DSO destructor observed)\n");
  return 0;
}
