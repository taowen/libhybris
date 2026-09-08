/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include "shaders/image-bounds.inc"
#include "../../hybris/vulkan/compat/spirv_image_bounds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_spv(const char *path, const uint32_t *words, size_t bytes)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    size_t wrote = fwrite(words, 1, bytes, file);
    fclose(file);
    return wrote == bytes;
}

static int validate(const char *path)
{
    char command[512];
    snprintf(command, sizeof(command), "spirv-val --target-env vulkan1.0 %s", path);
    return system(command) == 0;
}

static int select_operands_equal(const uint32_t *code, size_t words)
{
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 65535;
        if (!count || count > words - at) return 0;
        if (op == 169 && count >= 6 && code[at + 4] == code[at + 5]) return 1;
        at += count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t *out = NULL;
    size_t size = 0;
    unsigned rewritten = 0;
    VkResult result = hybris_spirv_image_bounds(kImageBounds, sizeof(kImageBounds), NULL, &out, &size, &rewritten);
    if (result != VK_SUCCESS || !out || !rewritten) {
        fprintf(stderr, "expected image-bounds rewrite: result=%d out=%p rewritten=%u\n",
            result, (void *)out, rewritten);
        return 1;
    }
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/image-bounds.spv", dir);
    if (!write_spv(path, out, size) || !validate(path)) {
        fprintf(stderr, "rewritten image-bounds module failed spirv-val\n");
        return 1;
    }
    if (!select_operands_equal(out, size / 4)) {
        fprintf(stderr, "OpSelect false object was not replaced with the true object\n");
        return 1;
    }
    free(out);
    printf("IMAGE_BOUNDS rewritten=%u spirv-val=ok select_collapsed=1\n", rewritten);
    return 0;
}
