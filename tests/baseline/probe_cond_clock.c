#include "probe.h"
#include <errno.h>

int cond_clock_probe(void) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*sym_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  if (!open_android || !sym_android || !close_android) return 2;
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) return 2;
  int (*timeout)(unsigned, long long *) = sym_android(fixture, "cond_fixture_timeout");
  if (!timeout) return 2;
  int failed = 0;
  for (unsigned variant = 0; variant < 7; ++variant) {
    long long elapsed = 0;
    int error = timeout(variant, &elapsed);
    printf("COND_CLOCK variant=%u result=%d elapsed_ns=%lld\n", variant, error, elapsed);
    /* Wrongly treating uptime as epoch time returns immediately. No tight
     * upper bound: scheduling delays are not clock-semantic failures. */
    if (variant < 3 ? (error != ETIMEDOUT || elapsed < 90000000) : error != EINVAL) failed = 1;
  }
  if (close_android(fixture)) failed = 1;
  printf("COND_CLOCK %s\n", failed ? "FAIL" : "PASS");
  return failed ? 2 : 0;
}
