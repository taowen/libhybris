#include "integrity_sha256.h"
#include <stdlib.h>
#include <string.h>

/* Without volatile, Clang may evaluate this simple constructor at link time,
 * remove .init_array, and fold the getter to 1 even when a loader skips it. */
static volatile int constructed;
int fixture_constructor_count(void) { return constructed; }

#ifdef INTEGRITY_FIXTURE
extern const unsigned char BORINGSSL_bcm_text_start[], BORINGSSL_bcm_text_end[];
extern const unsigned char BORINGSSL_bcm_rodata_start[], BORINGSSL_bcm_rodata_end[];
extern const unsigned char fixture_expected_hash[32] __attribute__((visibility("hidden")));
#ifndef MISSING_GETTER
const unsigned char *FIPS_module_hash(void) { return fixture_expected_hash; }
#endif
static void hash_length(hybris_sha256_context *ctx, size_t length) {
  unsigned char data[8];
  for (unsigned i = 0; i < 8; ++i) data[i] = (uint64_t)length >> (8 * i);
  hybris_sha256_update(ctx, data, sizeof(data));
}
int BORINGSSL_integrity_test(void) {
  unsigned char pad[64], inner[32], result[32];
  hybris_sha256_context ctx;
  memset(pad, 0x36, sizeof(pad));
  hybris_sha256_init(&ctx);
  hybris_sha256_update(&ctx, pad, sizeof(pad));
  size_t length = BORINGSSL_bcm_text_end - BORINGSSL_bcm_text_start;
#ifndef STATIC_FORMAT
  hash_length(&ctx, length);
#endif
  hybris_sha256_update(&ctx, BORINGSSL_bcm_text_start, length);
#ifndef STATIC_FORMAT
  length = BORINGSSL_bcm_rodata_end - BORINGSSL_bcm_rodata_start;
  hash_length(&ctx, length);
  hybris_sha256_update(&ctx, BORINGSSL_bcm_rodata_start, length);
#endif
  hybris_sha256_out(&ctx, inner);
  memset(pad, 0x5c, sizeof(pad));
  hybris_sha256_init(&ctx);
  hybris_sha256_update(&ctx, pad, sizeof(pad));
  hybris_sha256_update(&ctx, inner, sizeof(inner));
  hybris_sha256_out(&ctx, result);
  return memcmp(result, fixture_expected_hash, sizeof(result)) == 0;
}
#endif

__attribute__((constructor)) static void init(void) {
#ifdef INTEGRITY_FIXTURE
  if (!BORINGSSL_integrity_test()) abort();
#endif
  ++constructed;
}
