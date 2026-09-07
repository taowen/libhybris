/* Fixed palettes computed independently of the GPU decoder. Packing only:
 * this fixture is not a second implementation of the decode algorithm. */
#ifndef HYBRIS_BC_FIXTURE_H
#define HYBRIS_BC_FIXTURE_H
#include "compat/bc_decode.h"

static const VkFormat bc_formats[] = {
    VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK,
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
    VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK};

static unsigned bc_variant(unsigned bx, unsigned by, unsigned layer, unsigned round)
{ return (bx + 3 * by + 5 * layer + round) % 4; }
static uint32_t bc_golden(unsigned mode, unsigned variant, unsigned pixel, unsigned round)
{
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
    unsigned block_size = mode < 2 ? 8 : 16;
    memset(bytes, 0xa5, (size_t)r->source_range);
    for (unsigned z = 0; z < r->layers; ++z)
        for (unsigned y = 0; y < (r->height + 3) / 4; ++y)
            for (unsigned x = 0; x < (r->width + 3) / 4; ++x)
                bc_pack(bytes + r->source_offset + ((z * rows + y) * row + x) * block_size,
                    mode, bc_variant(x, y, z, round), round);
}
#endif
