#include "probe.h"
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

struct replay_args {
  void (*promote)(size_t, const void *, size_t, size_t);
  void *(*get_tls)(void);
  int passed;
};

static void *promote_on_worker(void *opaque) {
  struct replay_args *a = opaque;
  unsigned char second = 29;
  a->promote(901, &second, 1, 1);
  unsigned char *base = (unsigned char *)a->get_tls() - 8;
  a->passed = base[900] == 17 && base[901] == 29;
  printf("TLS_REPLAY worker prior=%u own=%u %s\n", base[900], base[901],
         a->passed ? "PASS" : "FAIL");
  return NULL;
}

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
  struct replay_args replay = { .promote = promote,
      .get_tls = dlsym(h, "_hybris_hook___get_tls_hooks") };
  if (!replay.get_tls) return 2;
  unsigned char first = 17;
  promote(900, &first, 1, 1);
  /* Catch-up must preserve mutations to entries already applied locally. */
  unsigned char *base = (unsigned char *)replay.get_tls() - 8;
  base[900] = 41;
  pthread_t worker;
  if (pthread_create(&worker, NULL, promote_on_worker, &replay)) return 2;
  if (pthread_join(worker, NULL) || !replay.passed) return 2;
  base = (unsigned char *)replay.get_tls() - 8;
  if (base[900] != 41 || base[901] != 29) return 2;
  printf("TLS_REPLAY main mutation=41 catchup=29 PASS\n");
  printf("TLS_BOUNDS PASS\n");
  return 0;
}
