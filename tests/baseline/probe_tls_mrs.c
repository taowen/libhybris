#include "probe.h"
#include <errno.h>
#include <sys/syscall.h>

struct mrs_worker { int (*read)(void); int ok, tid, observed; };
static void *first_mrs(void *opaque) {
  struct mrs_worker *w = opaque;
  w->tid = syscall(SYS_gettid);
  w->ok = 1;
  for (int i = 0; i < 32; ++i) {
    errno = E2BIG;
    w->observed = w->read();
    if (w->observed != w->tid || errno != E2BIG) { w->ok = 0; break; }
  }
  return NULL;
}
int tls_mrs_probe(void) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*symbol_android)(void *, const char *) = dlsym(common, "android_dlsym");
  if (!open_android || !symbol_android) return 2;
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  int (*read)(void) = fixture ? symbol_android(fixture, "tls_fixture_first_mrs") : NULL;
  if (!read) return 2;
  struct mrs_worker workers[8] = {0};
  pthread_t threads[8];
  unsigned started = 0;
  for (; started < 8; ++started) {
    workers[started].read = read;
    if (pthread_create(&threads[started], NULL, first_mrs, &workers[started])) break;
  }
  int failed = started != 8;
  for (unsigned i = 0; i < started; ++i) {
    if (pthread_join(threads[i], NULL)) return 2;
    printf("TLS_MRS thread=%u tid=%d observed=%d state=%s\n", i, workers[i].tid,
        workers[i].observed, workers[i].ok ? "preserved" : "FAIL");
    if (!workers[i].ok) failed = 1;
  }
  return failed ? 2 : 0;
}
