/* Independent finite binary16 arithmetic for image sampling references.
 * Values are exact signed multiples of 2^-24; no CPU BC decoder is used. */
#ifndef HYBRIS_BC_HALF_VERIFY_H
#define HYBRIS_BC_HALF_VERIFY_H
#include <stdint.h>
/* Exact widening, including signed zero and binary16 subnormals. */
static uint32_t bc_half_float_bits(uint16_t half)
{
    uint32_t sign = (uint32_t)(half & 32768) << 16;
    unsigned exponent = (half >> 10) & 31, fraction = half & 1023;
    if (exponent) return sign | ((exponent + 112) << 23) | (fraction << 13);
    if (!fraction) return sign;
    unsigned shift = 0;
    while (!(fraction & 1024)) { fraction <<= 1; ++shift; }
    return sign | ((113 - shift) << 23) | ((fraction & 1023) << 13);
}
static int64_t bc_half_units(uint16_t half)
{
    unsigned exponent = (half >> 10) & 31, fraction = half & 1023;
    int64_t magnitude = exponent ? (int64_t)(1024 + fraction) << (exponent - 1) : fraction;
    return half & 32768 ? -magnitude : magnitude;
}
static uint16_t bc_half_average(int64_t sum)
{
    unsigned sign = sum < 0 ? 32768 : 0;
    if (sum < 0) sum = -sum;
    unsigned low = 0, high = 0x7bff;
    while (low < high) {
        unsigned middle = (low + high) / 2;
        if (4 * bc_half_units(middle) < sum) low = middle + 1;
        else high = middle;
    }
    if (low) {
        int64_t upper = 4 * bc_half_units(low) - sum;
        int64_t lower = sum - 4 * bc_half_units(low - 1);
        if (lower < upper || (lower == upper && (low & 1))) --low;
    }
    return (uint16_t)(sign | low);
}
static int bc_half_filtered_matches(uint16_t actual, int64_t sum, int64_t maximum)
{
    unsigned exponent = (actual >> 10) & 31;
    if (exponent == 31) return 0; /* This corpus and its averages are finite. */
    int64_t error = 4 * bc_half_units(actual) - sum;
    if (error < 0) error = -error;
    int64_t step = INT64_C(1) << (exponent ? exponent - 1 : 0);
    /* Final binary16 nearest rounding plus two binary32 ulps at the largest
     * input scale for the four exact quarter-weight products and additions.
     * This is an ideal-filter reference bound, not a CTS certification. */
    int64_t arithmetic = maximum >> 23;
    return error <= 2 * step + 8 * arithmetic;
}
#endif
