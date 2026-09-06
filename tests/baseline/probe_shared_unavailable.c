#include "probe.h"
#include <errno.h>
#include <sys/stat.h>

int shared_unavailable_probe(void) {
  struct stat st;
  if (!stat("/dev/shm", &st) || errno != ENOENT) {
    printf("SHARED_UNAVAILABLE unsupported: requires missing glibc /dev/shm\n");
    return 3;
  }
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  uintptr_t (*allocate)(size_t) = dlsym(common, "hybris_shm_alloc");
  void *(*translate)(uintptr_t) = dlsym(common, "hybris_get_shmpointer");
  void *(*open_android)(const char *, int) = dlsym(common, "android_dlopen");
  void *(*sym_android)(void *, const char *) = dlsym(common, "android_dlsym");
  int (*close_android)(void *) = dlsym(common, "android_dlclose");
  if (!allocate || !translate || !open_android || !sym_android || !close_android) return 2;
  if (allocate(64) != 0) return 2;
  /* Known tagged offset zero: translation must also fail without backing. */
  if (translate(UINT64_C(0xffffffffff000000)) != NULL) return 2;
  printf("SHARED_UNAVAILABLE allocator and translator rejected missing backing\n");
  void *fixture = open_android("./libtls-fixture.so", RTLD_NOW);
  if (!fixture) return 2;
  int (*initialize)(unsigned) = sym_android(fixture, "shared_fixture_init");
  if (!initialize) return 2;
  for (unsigned kind = 0; kind < 3; ++kind) {
    int error = initialize(kind);
    printf("SHARED_UNAVAILABLE kind=%u result=%d expected=%d\n", kind, error, ENOMEM);
    if (error != ENOMEM) return 2;
  }
  if (close_android(fixture)) return 2;
  printf("SHARED_UNAVAILABLE PASS\n");
  return 0;
}
