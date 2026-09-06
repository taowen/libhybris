#include "probe.h"
#ifdef __BIONIC__
#include "stdio_fixture.h"
#endif

int stdio_probe(void) {
#ifdef __BIONIC__
  int (*flush)(unsigned) = stdio_flush_lifecycle;
#else
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*find_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  if (!open_android || !find_android || !close_android) return 2;
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) return 2;
  int (*flush)(unsigned) = find_android(fixture, "stdio_fixture_flush");
  if (!flush) return 2;
#endif
  for (unsigned mode = 0; mode < 2; ++mode) {
    printf("STDIO_FLUSH begin memory=%u\n", mode);
    int result = flush(mode);
    printf("STDIO_FLUSH memory=%u result=%d\n", mode, result);
    if (result) return 2;
  }
#ifndef __BIONIC__
  if (close_android(fixture)) return 2;
#endif
  printf("STDIO_FLUSH PASS\n");
  return 0;
}
