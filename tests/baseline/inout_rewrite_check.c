/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include "shaders/inout.inc"
#include "../../hybris/vulkan/compat/spirv_inout.h"
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

int main(int argc, char **argv)
{
    uint32_t *out = NULL;
    size_t size = 0;
    unsigned widened = 0;
    VkResult result = hybris_spirv_inout(kInoutVertex, sizeof(kInoutVertex),
        kInoutNarrow, sizeof(kInoutNarrow), NULL, &out, &size, &widened);
    if (result != VK_SUCCESS || !out || !size || !widened) {
        fprintf(stderr, "expected rewrite of vec4 vs vec2: result=%d out=%p size=%zu widened=%u\n",
            result, (void *)out, size, widened);
        return 1;
    }
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    snprintf(path, sizeof(path), "%s/inout-narrow.spv", dir);
    if (!write_spv(path, out, size) || !validate(path)) {
        fprintf(stderr, "rewritten narrow fragment failed spirv-val\n");
        return 1;
    }
    free(out);
    out = NULL; size = 0; widened = 0;
    result = hybris_spirv_inout(kInoutVertex, sizeof(kInoutVertex),
        kInoutMatch, sizeof(kInoutMatch), NULL, &out, &size, &widened);
    if (result != VK_SUCCESS || out || widened) {
        fprintf(stderr, "matching vec4/vec4 must not rewrite: result=%d out=%p widened=%u\n",
            result, (void *)out, widened);
        free(out);
        return 1;
    }
    out = NULL; size = 0; widened = 0;
    result = hybris_spirv_inout(kInoutVertex, sizeof(kInoutVertex),
        kInoutScalar, sizeof(kInoutScalar), NULL, &out, &size, &widened);
    if (result != VK_SUCCESS || !out || !widened) {
        fprintf(stderr, "expected rewrite of vec4 vs float: result=%d out=%p widened=%u\n",
            result, (void *)out, widened);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/inout-scalar.spv", dir);
    if (!write_spv(path, out, size) || !validate(path)) {
        fprintf(stderr, "rewritten scalar fragment failed spirv-val\n");
        return 1;
    }
    free(out);
    printf("INOUT_REWRITE widened_narrow=1 widened_scalar=1 matching=0 spirv-val=ok\n");
    return 0;
}
