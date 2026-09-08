/* Fixed image readback oracle for the existing BC probe. No Vulkan calls. */
#ifndef HYBRIS_BC_IMAGE_VERIFY_H
#define HYBRIS_BC_IMAGE_VERIFY_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc_fixture.h"

struct bc_image_readback {
    const uint32_t *actual;
    const uint8_t *source;
    struct hybris_bc_region region;
    uint32_t bytes, dynamic_offset, raw_offset, reference_offset;
    unsigned format, round, swizzle, patched, filtering, sampler_choice;
};

static int sampled_matches(uint32_t actual, uint32_t expected, int srgb)
{
    /* Standard sRGB EOTF, rounded to eight-bit linear output. Generated from
     * the transfer function, independently of the BC palettes/kernel. */
    static const uint8_t linear[256] = {
        0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3,
        4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7,
        8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 12, 12, 12, 13,
        13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 17, 18, 18, 19, 19, 20,
        20, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29,
        30, 30, 31, 32, 32, 33, 34, 35, 35, 36, 37, 37, 38, 39, 40, 41,
        41, 42, 43, 44, 45, 45, 46, 47, 48, 49, 50, 51, 51, 52, 53, 54,
        55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
        71, 72, 73, 74, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88,
        90, 91, 92, 93, 95, 96, 97, 99, 100, 101, 103, 104, 105, 107, 108, 109,
        111, 112, 114, 115, 116, 118, 119, 121, 122, 124, 125, 127, 128, 130, 131, 133,
        134, 136, 138, 139, 141, 142, 144, 146, 147, 149, 151, 152, 154, 156, 157, 159,
        161, 163, 164, 166, 168, 170, 171, 173, 175, 177, 179, 181, 183, 184, 186, 188,
        190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 220,
        222, 224, 226, 229, 231, 233, 235, 237, 239, 242, 244, 246, 248, 250, 253, 255,
    };
    for (unsigned component = 0; component < 4; ++component) {
        int want = (expected >> (8 * component)) & 255;
        if (srgb && component < 3) want = linear[want];
        int got = (actual >> (8 * component)) & 255;
        int tolerance = srgb && component < 3 ? 1 : 0;
        if (got - want > tolerance || want - got > tolerance) return 0;
    }
    return 1;
}
static unsigned bc_verify_image_readback(const struct bc_image_readback *check)
{
    unsigned f = check->format, mode = bc_mode(f), round = check->round;
    unsigned shape_index = check->swizzle, patched = check->patched, sampler_choice = check->sampler_choice;
    uint32_t width = check->region.width, height = check->region.height;
    uint32_t pixels = width * height * check->region.layers;
    uint32_t sample_words = f >= 8 ? 2 : 1, block_bytes = bc_block_bytes(mode);
    uint32_t columns = (width + 3) / 4, rows = (height + 3) / 4;
    uint32_t raw_size = columns * rows * check->region.layers * block_bytes;
    uint32_t source_columns = ((check->region.row_length ?: width) + 3) / 4;
    uint32_t source_rows = ((check->region.image_height ?: height) + 3) / 4;
    uint32_t BYTES = check->bytes, RAW = check->raw_offset, dynamic_offset = check->dynamic_offset;
    const uint32_t *actual = check->actual;
    const uint32_t *reference_pixels = (const uint32_t *)(check->source + check->reference_offset);
    unsigned bad = 0;
    for (unsigned word = 0; word < BYTES / 4; ++word) {
        uint32_t expected = 0xcdcdcdcd;
        int matches;
        if (word >= dynamic_offset / 4 && word < dynamic_offset / 4 + sample_words * pixels) {
            expected = actual[word + sample_words * pixels];
            matches = actual[word] == expected;
        } else if (word >= dynamic_offset / 4 + sample_words * pixels && word < dynamic_offset / 4 + 2 * sample_words * pixels) {
            unsigned sample_word = word - dynamic_offset / 4 - sample_words * pixels;
            unsigned pixel = sample_word / sample_words;
            unsigned x = pixel % width, y = pixel / width % height, z = pixel / (width * height);
            expected = bc_golden(mode, bc_variant(mode, x / 4, y / 4, z, round), (y & 3) * 4 + (x & 3), round);
            if (patched && z == 1 && x >= 4 && x < 8 && y < 4) expected = f < 4 ? 0xff000000 : 0;
            if (f >= 8) {
                uint32_t red = expected & 65535, green = expected >> 16;
                uint32_t alpha = f & 1 ? 32767 : 65535;
                expected = sample_word & 1 ? ((shape_index ? red : 0) | (alpha << 16)) :
                    ((shape_index ? 0 : red) | (green << 16));
                matches = actual[word] == expected;
            } else {
                if (shape_index) expected = (expected & 0xff00ff00) | ((expected & 255) << 16) | ((expected >> 16) & 255);
                matches = sampled_matches(actual[word], expected, f & 1);
            }
        } else if (check->filtering && word >= dynamic_offset / 4 + 2 * sample_words * pixels &&
            word < dynamic_offset / 4 + 3 * sample_words * pixels) {
            expected = actual[word + sample_words * pixels];
            matches = actual[word] == expected;
        } else if (check->filtering && word >= dynamic_offset / 4 + 3 * sample_words * pixels &&
            word < dynamic_offset / 4 + 4 * sample_words * pixels) {
            unsigned sample_word = word - dynamic_offset / 4 - 3 * sample_words * pixels;
            unsigned pixel = sample_word / sample_words;
            unsigned x = pixel % width, y = pixel / width % height, z = pixel / (width * height);
            if (f < 2) {
                /* Native RGBA filtering supplies the RGB oracle. Force
                 * its view alpha to one to model the missing BC1 RGB
                 * component, including transparent-black borders. */
                expected = (actual[word] & 0x00ffffff) | 0xff000000;
                matches = actual[word] == expected;
            } else {
                /* The chosen coordinates average four adjacent texels with
                 * exact half weights. Check native RG16 filtering against
                 * that average; the BC-to-native comparison above is exact. */
                int sums[4] = {0};
                for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
                    unsigned sx = x + dx < width ? x + dx : width - 1;
                    unsigned sy = y + dy < height ? y + dy : height - 1;
                    uint32_t packed = bc_reference_load(reference_pixels, f, (z * height + sy) * width + sx);
                    if (sampler_choice >= 2 && (x + dx >= width || y + dy >= height)) {
                        uint32_t maximum = f & 1 ? 32767 : 65535;
                        packed = sampler_choice == 2 ? maximum | (mode >= 6 ? maximum << 16 : 0) : 0;
                    }
                    int red = f & 1 ? (int16_t)packed : (int)(packed & 65535);
                    int green = f & 1 ? (int16_t)(packed >> 16) : (int)(packed >> 16);
                    sums[shape_index ? 2 : 0] += red;
                    sums[1] += green;
                    sums[3] += f & 1 ? 32767 : 65535;
                }
                matches = 1; expected = 0;
                for (unsigned c = 0; c < 2; ++c) {
                    unsigned component = 2 * (sample_word & 1) + c;
                    int sum = sums[component];
                    int want = sum < 0 ? -((-sum + 2) / 4) : (sum + 2) / 4;
                    int got = f & 1 ? (int16_t)(actual[word] >> (16 * c)) :
                        (int)((actual[word] >> (16 * c)) & 65535);
                    int tolerance = component == 3 || component == (shape_index ? 0 : 2) ? 0 : 1;
                    if (abs(got - want) > tolerance) matches = 0;
                    expected |= (uint32_t)(uint16_t)want << (16 * c);
                }
            }
        } else if (word >= RAW / 4 && word < (RAW + raw_size) / 4) {
            unsigned byte = (word - RAW / 4) * 4;
            unsigned block = byte / block_bytes, x = block % columns, y = block / columns % rows, z = block / (columns * rows);
            memcpy(&expected, check->source + check->region.source_offset + ((z * source_rows + y) * source_columns + x) * block_bytes + byte % block_bytes, 4);
            if (patched && z == 1 && x == 1 && y == 0) expected = 0;
            matches = actual[word] == expected;
        } else matches = actual[word] == expected;
        if (!matches) {
            if (bad < 3) printf("BC_IMAGES_MISMATCH word=%u actual=%08x expected=%08x srgb=%u\n", word, actual[word], expected, f & 1);
            ++bad;
        }
    }
    return bad;
}
#endif
