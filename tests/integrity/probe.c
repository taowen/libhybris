#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void *(*open_library)(const char *, int) = dlopen;
static void *(*lookup)(void *, const char *) = dlsym;
static char *(*last_error)(void) = dlerror;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int hash_is_readonly(void *library) {
  const unsigned char *(*getter)(void) = lookup(library, "FIPS_module_hash");
  if (!getter) return 1; /* Old releases have no public accessor. */
  uintptr_t address = (uintptr_t)getter();
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps) return 0;
  char line[1024], flags[5];
  unsigned long start, end;
  int valid = 0;
  while (fgets(line, sizeof(line), maps)) {
    if (sscanf(line, "%lx-%lx %4s", &start, &end, flags) == 3 &&
        start <= address && address + 32 <= end) {
      valid = flags[0] == 'r' && flags[1] == '-' && flags[3] == 'p';
      break;
    }
  }
  fclose(maps);
  return valid;
}

static int (*random_bytes)(unsigned char *, size_t);
static unsigned char *(*sha256)(const unsigned char *, size_t, unsigned char *);
static int crypto_worker(void) {
  static const unsigned char expected[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  unsigned char digest[32], random[32], previous[32] = {0};
  for (int i = 0; i < 32; ++i) {
    REQUIRE(sha256((const unsigned char *)"abc", 3, digest));
    REQUIRE(!memcmp(digest, expected, 32));
    REQUIRE(random_bytes(random, sizeof(random)) == 1);
    REQUIRE(memcmp(random, previous, sizeof(random)));
    memcpy(previous, random, sizeof(random));
  }
  return 0;
}
static void *worker(void *unused) { (void)unused; return (void *)(uintptr_t)crypto_worker(); }

static int real_crypto(void) {
  void *crypto = open_library("/system/lib64/libcrypto.so", RTLD_NOW);
  if (!crypto) fprintf(stderr, "%s\n", last_error());
  REQUIRE(crypto);
  REQUIRE(hash_is_readonly(crypto));
  int (*integrity)(void) = lookup(crypto, "BORINGSSL_integrity_test");
  int (*self_test)(void) = lookup(crypto, "BORINGSSL_self_test");
  if (integrity) REQUIRE(integrity() == 1);
  if (self_test) REQUIRE(self_test() == 1);
  random_bytes = lookup(crypto, "RAND_bytes");
  sha256 = lookup(crypto, "SHA256");
  REQUIRE(random_bytes && sha256);
  REQUIRE(!crypto_worker());
  pthread_t threads[8];
  for (int i = 0; i < 8; ++i) REQUIRE(!pthread_create(&threads[i], NULL, worker, NULL));
  for (int i = 0; i < 8; ++i) {
    void *result;
    REQUIRE(!pthread_join(threads[i], &result));
    REQUIRE(!result);
  }
  void *ssl = open_library("/system/lib64/libssl.so", RTLD_NOW);
  if (!ssl) fprintf(stderr, "%s\n", last_error());
  REQUIRE(ssl);
  const void *(*method)(void) = lookup(ssl, "TLS_method");
  void *(*ctx_new)(const void *) = lookup(ssl, "SSL_CTX_new");
  void (*ctx_free)(void *) = lookup(ssl, "SSL_CTX_free");
  REQUIRE(method && ctx_new && ctx_free);
  void *ctx = ctx_new(method());
  REQUIRE(ctx);
  ctx_free(ctx);
  printf("PASS system crypto: integrity=%s self_test=%s SHA256/RAND 9 threads, SSL_CTX\n",
      integrity ? "1" : "absent", self_test ? "1" : "absent");
  return 0;
}

static int fixture(const char *path, const char *mode) {
  void *lib = open_library(path, RTLD_NOW);
  if (!strcmp(mode, "reject")) {
    REQUIRE(!lib);
    const char *error = last_error();
    REQUIRE(error && strstr(error, "module integrity:"));
    printf("PASS rejected: %s\n", error);
    return 0;
  }
  if (!lib) fprintf(stderr, "%s\n", last_error());
  REQUIRE(lib);
  int (*count)(void) = lookup(lib, "fixture_constructor_count");
  REQUIRE(count && count() == 1);
  int (*integrity)(void) = lookup(lib, "BORINGSSL_integrity_test");
  if (integrity) {
    REQUIRE(hash_is_readonly(lib));
    REQUIRE(integrity() == 1);
    unsigned char *code = lookup(lib, "BORINGSSL_bcm_text_start");
    long size = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)code & ~((uintptr_t)size - 1));
    /* Android forbids adding X to a modified file mapping. Use a private
     * anonymous copy for deliberate test-only code corruption. */
    unsigned char *saved = malloc(size);
    REQUIRE(saved);
    memcpy(saved, page, size);
    REQUIRE(mmap(page, size, PROT_READ | PROT_WRITE,
        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == page);
    memcpy(page, saved, size);
    free(saved);
    REQUIRE(!mprotect(page, size, PROT_READ | PROT_WRITE));
    code[0] ^= 1;
    REQUIRE(!mprotect(page, size, PROT_READ | PROT_EXEC));
    REQUIRE(integrity() == 0);
    REQUIRE(!mprotect(page, size, PROT_READ | PROT_WRITE));
    code[0] ^= 1;
    REQUIRE(!mprotect(page, size, PROT_READ | PROT_EXEC));
    REQUIRE(integrity() == 1);
  }
  printf("PASS %s constructors=1 integrity=%s\n", path, integrity ? "1,0,1 after tamper/restore" : "absent");
  return 0;
}
int main(int argc, char **argv) {
  REQUIRE(argc >= 3);
  setbuf(stdout, NULL);
  if (!strcmp(argv[1], "hybris")) {
    void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!common) fprintf(stderr, "%s\n", dlerror());
    REQUIRE(common);
    open_library = dlsym(common, "android_dlopen");
    lookup = dlsym(common, "android_dlsym");
    last_error = dlsym(common, "android_dlerror");
    REQUIRE(open_library && lookup && last_error);
  }
  if (!strcmp(argv[2], "system")) return real_crypto();
  REQUIRE(argc == 4);
  return fixture(argv[2], argv[3]);
}
