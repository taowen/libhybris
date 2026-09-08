/* Offline Mesa reference adapter. No float conversion is used by the selected
 * fp16 entry point; declarations below belong to unused static helpers. */
#define _DEFAULT_SOURCE
#include <strings.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define FP16_ONE 0x3c00
extern float _mesa_half_to_float(uint16_t);
extern uint16_t _mesa_float_to_half(float);
static int64_t util_sign_extend(uint64_t value, unsigned bits) {
 assert(bits > 0 && bits < 32 && value < (UINT64_C(1) << bits));
 return (int64_t)(value ^ (UINT64_C(1) << (bits - 1))) - (INT64_C(1) << (bits - 1));
}
#include "texcompress_bptc_tmp.h"
void decode(const void *input, void *output, int sign) {
 decompress_rgb_fp16_block(4, 4, input, output, 32, sign != 0);
}
