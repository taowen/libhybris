/* SPDX-License-Identifier: Apache-2.0 */
#include "spirv_decorations.h"
#include <string.h>

struct group { size_t first, last, words, decorations; unsigned defined; };
VkResult hybris_spirv_decorations(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason)
{
    *output = NULL; *output_size = 0;
    *reason = "unsupported or malformed SPIR-V decoration group";
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_ERROR_UNKNOWN;
    size_t words = size / 4, applications = 0;
    uint32_t bound = code[3];
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || count > words - at) return VK_ERROR_UNKNOWN;
        applications += op == 74 || op == 75;
        at += count;
    }
    if (!applications) { *reason = NULL; return VK_SUCCESS; }
    if (sizeof(struct group) > SIZE_MAX / bound || sizeof(size_t) > SIZE_MAX / words)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct group *groups = hybris_scaled_alloc(allocator, bound * sizeof(*groups), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    size_t *next = hybris_scaled_alloc(allocator, words * sizeof(*next), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    uint32_t *result = NULL;
    VkResult status = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!groups || !next) goto done;
    memset(groups, 0, bound * sizeof(*groups));
    memset(next, 0, words * sizeof(*next));
    status = VK_ERROR_UNKNOWN;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        if ((p[0] & 0xffff) != 73) continue;
        if (p[0] >> 16 != 2 || !p[1] || p[1] >= bound || groups[p[1]].defined) goto done;
        groups[p[1]].defined = 1;
    }
    /* Retain the group definitions and their annotations. Replacing only their
     * applications preserves names/debug references to the original group IDs;
     * subsequent passes consume the expanded target decorations. */
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
        if (op != 71 && op != 332 && op != 5632) continue;
        if (count < 3 || p[1] >= bound) goto done;
        struct group *group = &groups[p[1]];
        if (!group->defined) continue;
        if (op == 5632) goto done;
        if (group->last) next[group->last] = at;
        else group->first = at;
        group->last = at;
        if (count > SIZE_MAX - group->words) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        group->words += count; ++group->decorations;
    }
    size_t capacity = words;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
        if (op != 74 && op != 75) continue;
        if (count < 2 || p[1] >= bound || !groups[p[1]].defined || (op == 75 && count % 2)) goto done;
        const struct group *group = &groups[p[1]];
        unsigned stride = op == 75 ? 2 : 1;
        for (uint32_t i = 2; i < count; i += stride)
            if (!p[i] || p[i] >= bound || groups[p[i]].defined) goto done;
        size_t expanded = group->words;
        if (op == 75) {
            for (size_t d = group->first; d; d = next[d])
                if ((code[d] & 0xffff) != 71 || code[d] >> 16 == 0xffff) goto done;
            if (group->decorations > SIZE_MAX - expanded) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            expanded += group->decorations;
        }
        size_t targets = (count - 2) / stride;
        if (targets && expanded > (SIZE_MAX - capacity) / targets) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        capacity += expanded * targets;
    }
    if (capacity > SIZE_MAX / 4) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    result = hybris_scaled_alloc(allocator, capacity * 4, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!result) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(result, code, 20);
    size_t out = 5;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
        if (op != 74 && op != 75) { memcpy(result + out, p, count * 4); out += count; continue; }
        unsigned stride = op == 75 ? 2 : 1;
        for (uint32_t i = 2; i < count; i += stride)
            for (size_t d = groups[p[1]].first; d; d = next[d]) {
                const uint32_t *decoration = code + d;
                uint32_t n = decoration[0] >> 16;
                result[out++] = op == 75 ? ((n + 1) << 16) | 72 : decoration[0];
                result[out++] = p[i];
                if (op == 75) result[out++] = p[i + 1];
                memcpy(result + out, decoration + 2, (n - 2) * 4); out += n - 2;
            }
    }
    *output = result; *output_size = out * 4; result = NULL;
    status = VK_SUCCESS; *reason = NULL;
done:
    hybris_scaled_free(allocator, result);
    hybris_scaled_free(allocator, next);
    hybris_scaled_free(allocator, groups);
    return status;
}
