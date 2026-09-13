/* Private SHA-256 interface for loader integrity checks. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  unsigned char buf[64];
  uint64_t count;
  uint32_t val[8];
} hybris_sha256_context;
void hybris_sha256_init(hybris_sha256_context *ctx);
void hybris_sha256_update(hybris_sha256_context *ctx, const void *data, size_t size);
void hybris_sha256_out(const hybris_sha256_context *ctx, unsigned char out[32]);
#ifdef __cplusplus
}
#endif
