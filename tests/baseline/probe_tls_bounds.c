#include "probe.h"
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

/* Isolated children deliberately call the linker callback with corrupt metadata.
 * No Android library or GPU object is created in this process. */
int tls_bounds_probe(void) {
  void *h = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("common load failed: %s\n", dlerror()); return 2; }
  void (*promote)(size_t, const void *, size_t, size_t) =
      dlsym(h, "_hybris_init_static_tls_for_thread");
  if (!promote) return 2;
  const size_t inputs[][3] = {
      {SIZE_MAX, 0, 2}, /* offset+memsz would wrap */
      {1023, 2, 1},    /* filesz exceeds reserved range */
      {1024, 0, 1},    /* memsz past end */
      {72, 1, 1},     /* NULL source with nonzero filesz */
  };
  for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
      struct rlimit zero = {0, 0};
      setrlimit(RLIMIT_CORE, &zero);
      alarm(5);
      char bytes[2] = {1, 2};
      promote(inputs[i][0], i == 3 ? NULL : bytes, inputs[i][1], inputs[i][2]);
      _exit(0);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
      printf("TLS_BOUNDS rejection case=%u FAIL\n", i); return 2;
    }
    printf("TLS_BOUNDS rejection case=%u PASS\n", i);
  }
  /* The exact end with an empty segment is legal and must not be rejected. */
  promote(1024, NULL, 0, 0);
  printf("TLS_BOUNDS PASS\n");
  return 0;
}
