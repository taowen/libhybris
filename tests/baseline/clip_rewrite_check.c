/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include "shaders/clip.inc"
#include "../../hybris/vulkan/compat/clip_distance.h"
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

static int has_clip_capability(const uint32_t *code, size_t bytes)
{
    size_t words = bytes / 4;
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 65535;
        if (!count || count > words - at) return 0;
        if (op == 17 && count == 2 && code[at + 1] == 32) return 1;
        at += count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t location = 0, count = 0;
    uint32_t *vs = NULL, *fs = NULL;
    size_t vs_size = 0, fs_size = 0;
    unsigned vs_made = 0, fs_made = 0;
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[512];
    VkResult result = hybris_spirv_clip_plan(kClipVertex, sizeof(kClipVertex),
        kClipFragment, sizeof(kClipFragment), &location, &count);
    if (result != VK_SUCCESS || count != 6 || location == 0xffffffffu) {
        fprintf(stderr, "plan failed result=%d count=%u location=%u\n", result, count, location);
        return 1;
    }
    result = hybris_spirv_clip_vertex(kClipVertex, sizeof(kClipVertex), location, NULL,
        &vs, &vs_size, &vs_made);
    if (result != VK_SUCCESS || !vs || !vs_made || has_clip_capability(vs, vs_size)) {
        fprintf(stderr, "vertex rewrite failed result=%d made=%u cap=%d\n",
            result, vs_made, vs ? has_clip_capability(vs, vs_size) : -1);
        free(vs);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/clip-vs.spv", dir);
    if (!write_spv(path, vs, vs_size) || !validate(path)) {
        fprintf(stderr, "rewritten vertex failed spirv-val\n");
        free(vs);
        return 1;
    }
    result = hybris_spirv_clip_fragment(kClipFragment, sizeof(kClipFragment), location, count,
        NULL, &fs, &fs_size, &fs_made);
    if (result != VK_SUCCESS || !fs || !fs_made) {
        fprintf(stderr, "fragment rewrite failed result=%d made=%u\n", result, fs_made);
        free(vs);
        free(fs);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/clip-fs.spv", dir);
    if (!write_spv(path, fs, fs_size) || !validate(path)) {
        fprintf(stderr, "rewritten fragment failed spirv-val\n");
        free(vs);
        free(fs);
        return 1;
    }
    printf("CLIP_REWRITE count=%u location=%u vs=%zu fs=%zu spirv-val=ok\n",
        count, location, vs_size, fs_size);
    free(vs);
    free(fs);
    return 0;
}
