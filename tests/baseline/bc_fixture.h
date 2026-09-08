/* Fixed BC1-5 palettes and independently decoded BC7 corpus. Packing only:
 * no CPU decoder is linked into the probe or production path. */
#ifndef HYBRIS_BC_FIXTURE_H
#define HYBRIS_BC_FIXTURE_H
#include "compat/bc_decode.h"
#include "bc7_fixture.inc"

static const VkFormat bc_formats[] = {
    VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK,
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
    VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
    VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
    VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK};
#define BC_FORMAT_COUNT (sizeof(bc_formats) / sizeof(bc_formats[0]))
static unsigned bc_mode(unsigned format_index)
{ return format_index >= 12 ? 9 : format_index < 8 ? format_index / 2 : format_index - 4; }
static unsigned bc_block_bytes(unsigned mode)
{ return mode < 2 || mode == 4 || mode == 5 ? 8 : 16; }
static inline VkFormat bc_reference_format(unsigned f)
{
    if (f == 8 || f == 9) return f & 1 ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_UNORM;
    if (f >= 8 && f < 12) return f & 1 ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
    return f & 1 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

static unsigned bc_variant(unsigned mode, unsigned bx, unsigned by, unsigned layer, unsigned round)
{
    if (mode == 9) return (bx + 32 * by + 137 * layer + 73 * round) % BC7_VECTOR_COUNT;
    return (bx + 3 * by + 5 * layer + round) % (mode >= 4 ? 8 : 4);
}
static inline uint32_t bc_fill_word(unsigned mode)
{ return mode == 9 ? 0x40u : 0u; }
static inline uint32_t bc_fill_golden(unsigned mode, unsigned pixel)
{ return mode == 9 ? bc7_fill_rgba[pixel] : mode < 2 ? 0xff000000u : 0u; }
/* BC4/5 fixed RGTC palettes, rounded once to normalized 16-bit values.
 * Unsigned endpoints: 210/0, 0/200, 100/100, 203/13, 1/0, 254/255,
 * 255/255, 0/0. Signed: 100/-110, -100/100, -128/-128, 103/-13,
 * -1/0, 127/-128, -128/-127, -127/-127. The undefined -127/-128
 * ordering is deliberately excluded from the portable golden fixture. */
static uint16_t bc_channel_golden(unsigned snorm, unsigned variant, unsigned index)
{
    static const int32_t palette[2][8][8] = {
    {
        {53970, 0, 46260, 38550, 30840, 23130, 15420, 7710},
        {0, 51400, 10280, 20560, 30840, 41120, 0, 65535},
        {25700, 25700, 25700, 25700, 25700, 25700, 0, 65535},
        {52171, 3341, 45195, 38220, 31244, 24268, 17292, 10317},
        {257, 0, 220, 184, 147, 110, 73, 37},
        {65278, 65535, 65329, 65381, 65432, 65484, 0, 65535},
        {65535, 65535, 65535, 65535, 65535, 65535, 0, 65535},
        {0, 0, 0, 0, 0, 0, 0, 65535}},
    {
        {25801, -28381, 18061, 10320, 2580, -5160, -12900, -20641},
        {-25801, 25801, -15480, -5160, 5160, 15480, -32767, 32767},
        {-32767, -32767, -32767, -32767, -32767, -32767, -32767, 32767},
        {26575, -3354, 22299, 18024, 13748, 9473, 5197, 921},
        {-258, 0, -206, -155, -103, -52, -32767, 32767},
        {32767, -32767, 23405, 14043, 4681, -4681, -14043, -23405},
        {-32767, -32767, -32767, -32767, -32767, -32767, -32767, 32767},
        {-32767, -32767, -32767, -32767, -32767, -32767, -32767, 32767}}};
    return (uint16_t)palette[snorm][variant][index];
}
static void bc_pack_channel(uint8_t *destination, unsigned snorm, unsigned variant, unsigned round)
{
    static const int16_t endpoints[2][8][2] = {
        {{210,0},{0,200},{100,100},{203,13},{1,0},{254,255},{255,255},{0,0}},
        {{100,-110},{-100,100},{-128,-128},{103,-13},{-1,0},{127,-128},{-128,-127},{-127,-127}}};
    uint64_t bits = (uint8_t)endpoints[snorm][variant][0] |
        ((uint64_t)(uint8_t)endpoints[snorm][variant][1] << 8);
    for (unsigned pixel = 0; pixel < 16; ++pixel)
        bits |= (uint64_t)((pixel + variant + round) & 7) << (16 + pixel * 3);
    memcpy(destination, &bits, 8);
}
static uint32_t bc_golden(unsigned mode, unsigned variant, unsigned pixel, unsigned round)
{
    if (mode == 9) return bc7_vectors[variant].rgba[pixel];
    if (mode >= 4) {
        uint32_t red = bc_channel_golden(mode & 1, variant, (pixel + variant + round) & 7);
        unsigned green_variant = (variant + 3) & 7;
        uint32_t green = mode >= 6 ? bc_channel_golden(mode & 1, green_variant,
            (pixel + green_variant + round + 2) & 7) : 0;
        return red | (green << 16);
    }
    /* RGB565 red/green; black/white with c0 <= c1; equal mixed endpoints.
     * Mixed endpoint = (11 << 11) | (29 << 5) | 21 = 0x5bb5. */
    static const uint32_t palettes[4][4] = {
        {0x0000ff, 0x00ff00, 0x0055aa, 0x00aa55},
        {0x000000, 0xffffff, 0x7f7f7f, 0x000000},
        {0xad755a, 0xad755a, 0xad755a, 0x000000},
        {0xad755a, 0x397921, 0x867647, 0x5f7734}};
    unsigned index = (pixel + variant + round) & 3;
    uint32_t color = palettes[variant][index];
    uint32_t alpha = mode == 1 && (variant == 1 || variant == 2) && index == 3 ? 0 : 255;
    if (mode >= 2 && variant == 1 && index >= 2)
        color = index == 2 ? 0x555555 : 0xaaaaaa;
    if (mode >= 2 && variant == 2) color = 0xad755a;
    if (mode == 2) alpha = ((pixel + variant + round) & 15) * 17;
    if (mode == 3) {
        static const uint8_t alpha_palettes[4][8] = {
            {210, 0, 180, 150, 120, 90, 60, 30},
            {0, 200, 40, 80, 120, 160, 0, 255},
            {100, 100, 100, 100, 100, 100, 0, 255},
            {203, 13, 175, 148, 121, 94, 67, 40}};
        alpha = alpha_palettes[variant][(pixel + variant + round) & 7];
    }
    return color | (alpha << 24);
}
static void bc_pack(uint8_t *destination, unsigned mode, unsigned variant, unsigned round)
{
    if (mode == 9) { memcpy(destination, bc7_vectors[variant].block, 16); return; }
    if (mode >= 4) {
        bc_pack_channel(destination, mode & 1, variant, round);
        if (mode >= 6) bc_pack_channel(destination + 8, mode & 1, (variant + 3) & 7, round + 2);
        return;
    }
    static const uint16_t endpoints[4][2] = {{0xf800, 0x07e0}, {0x0000, 0xffff}, {0x5bb5, 0x5bb5}, {0x5bb5, 0x23c7}};
    uint32_t words[4] = {0};
    unsigned color = mode < 2 ? 0 : 2;
    words[color] = endpoints[variant][0] | ((uint32_t)endpoints[variant][1] << 16);
    uint64_t alpha = 0;
    if (mode == 3) {
        static const uint16_t alpha_endpoints[4] = {210, 200 << 8, 100 | (100 << 8), 203 | (13 << 8)};
        alpha = alpha_endpoints[variant];
    }
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        words[color + 1] |= ((pixel + variant + round) & 3) << (pixel * 2);
        if (mode == 2) alpha |= (uint64_t)((pixel + variant + round) & 15) << (pixel * 4);
        if (mode == 3) alpha |= (uint64_t)((pixel + variant + round) & 7) << (16 + pixel * 3);
    }
    if (mode >= 2) { words[0] = (uint32_t)alpha; words[1] = (uint32_t)(alpha >> 32); }
    memcpy(destination, words, mode < 2 ? 8 : 16);
}
static void bc_fill_fixture(uint8_t *bytes, const struct hybris_bc_region *r,
    unsigned mode, unsigned round)
{
    unsigned row = ((r->row_length ?: r->width) + 3) / 4;
    unsigned rows = ((r->image_height ?: r->height) + 3) / 4;
    unsigned block_size = bc_block_bytes(mode);
    memset(bytes, 0xa5, (size_t)r->source_range);
    for (unsigned z = 0; z < r->layers; ++z)
        for (unsigned y = 0; y < (r->height + 3) / 4; ++y)
            for (unsigned x = 0; x < (r->width + 3) / 4; ++x)
                bc_pack(bytes + r->source_offset + ((z * rows + y) * row + x) * block_size,
                    mode, bc_variant(mode, x, y, z, round), round);
}

/* Native reference storage preserves the original channel count, including
 * border replacement before missing-component substitution. */
static inline void bc_reference_store(void *data, unsigned f, unsigned pixel, uint32_t value)
{
    if (f == 8 || f == 9) ((uint16_t *)data)[pixel] = (uint16_t)value;
    else ((uint32_t *)data)[pixel] = value;
}
static inline uint32_t bc_reference_load(const void *data, unsigned f, unsigned pixel)
{
    if (f == 8 || f == 9) return ((const uint16_t *)data)[pixel];
    return ((const uint32_t *)data)[pixel];
}
#endif
