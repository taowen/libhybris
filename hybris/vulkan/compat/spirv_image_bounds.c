/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "spirv_image_bounds.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int enabled;
static void configure(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_COMPAT_IMAGE_BOUNDS");
    enabled = value && !strcmp(value, "1");
}
int hybris_image_bounds_enabled(void) { pthread_once(&once, configure); return enabled; }

static int is_zero(const uint32_t *code, size_t words, uint32_t id, unsigned depth)
{
    if (!id || depth > 8) return 0;
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 65535;
        if (!count || count > words - at) return 0;
        if (op == 43 && count >= 4 && code[at + 2] == id) {
            if (code[at + 3] != 0) return 0;
            if (count > 4 && code[at + 4] != 0) return 0;
            return 1;
        }
        if (op == 44 && count >= 3 && code[at + 2] == id) {
            for (uint32_t i = 3; i < count; ++i)
                if (!is_zero(code, words, code[at + i], depth + 1)) return 0;
            return count > 3;
        }
        at += count;
    }
    return 0;
}

VkResult hybris_spirv_image_bounds(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *rewritten)
{
    *output = NULL; *output_size = 0; *rewritten = 0;
    if (size < 20 || size % 4 || code[0] != 0x07230203) return VK_SUCCESS;
    size_t words = size / 4;
    unsigned hits = 0;
    int watch = 0;
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 65535;
        if (!count || count > words - at) return VK_SUCCESS;
        if (op == 86 || op == 95) watch = 1;
        else if (op == 62) watch = 0;
        else if (op == 169 && watch && count >= 6) {
            if (is_zero(code, words, code[at + 5], 0) && code[at + 4] != code[at + 5]) ++hits;
            watch = 0;
        }
        at += count;
    }
    if (!hits) return VK_SUCCESS;
    uint32_t *result = hybris_scaled_alloc(allocator, size, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!result) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(result, code, size);
    watch = 0;
    unsigned made = 0;
    for (size_t at = 5; at < words;) {
        uint32_t count = result[at] >> 16, op = result[at] & 65535;
        if (op == 86 || op == 95) watch = 1;
        else if (op == 62) watch = 0;
        else if (op == 169 && watch && count >= 6) {
            if (is_zero(result, words, result[at + 5], 0) && result[at + 4] != result[at + 5]) {
                result[at + 5] = result[at + 4];
                ++made;
            }
            watch = 0;
        }
        at += count;
    }
    if (!made) { hybris_scaled_free(allocator, result); return VK_SUCCESS; }
    *output = result;
    *output_size = size;
    *rewritten = made;
    return VK_SUCCESS;
}
