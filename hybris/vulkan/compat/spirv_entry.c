/* SPDX-License-Identifier: Apache-2.0 */
#include "spirv_entry.h"
#include <string.h>

static const unsigned char result_positions[] = {
#include "spirv_results.inc"
};
struct definition { uint32_t owner, opcode; unsigned live; };

int hybris_spirv_multiple(const uint32_t *code, size_t size)
{
    if (size < 20 || size % 4 || code[0] != 0x07230203) return 0;
    unsigned entries = 0;
    for (size_t at = 5; at < size / 4;) {
        uint32_t count = code[at] >> 16;
        if (!count || count > size / 4 - at) return 0;
        entries += (code[at] & 0xffff) == 15;
        at += count;
    }
    return entries > 1;
}

/* Extract the selected entry's ordinary logical-shader call tree. Types and
 * constants are retained. Global variable liveness is conservative: a literal
 * equal to a global variable ID can retain extra declarations, never delete a
 * required one. Function-pointer extensions and nonsemantic debug references
 * into removed functions are explicitly unsupported. */
VkResult hybris_spirv_entry(const uint32_t *code, size_t size, uint32_t model,
    const char *entry, const VkAllocationCallbacks *allocator, uint32_t **output,
    size_t *output_size, const char **reason)
{
    *output = NULL; *output_size = 0;
    *reason = "unsupported or malformed SPIR-V entry extraction";
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_ERROR_UNKNOWN;
    uint32_t bound = code[3];
    if (sizeof(struct definition) > SIZE_MAX / bound) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct definition *ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    uint32_t *result = NULL;
    VkResult status = VK_ERROR_UNKNOWN;
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(ids, 0, bound * sizeof(*ids));
    uint32_t function = 0, selected = 0;
    size_t selected_entry = 0, words = size / 4;
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (!count || count > words - at || op >= sizeof(result_positions) || !result_positions[op]) goto done;
        if (op == 5600 || op == 5601) { *reason = "SPIR-V function pointers are unsupported"; goto done; }
        if (op == 54) { if (count < 5 || p[2] >= bound) goto done; function = p[2]; }
        unsigned position = result_positions[op] - 1;
        if (position) {
            if (position >= count || !p[position] || p[position] >= bound) goto done;
            ids[p[position]].owner = function;
            ids[p[position]].opcode = op;
            ids[p[position]].live = !function && op != 59;
        }
        if (op == 15) {
            if (count < 4 || p[2] >= bound) goto done;
            const char *name = (const char *)(p + 3);
            const char *end = memchr(name, 0, (count - 3) * 4);
            if (!end) goto done;
            if (p[1] == model && !strcmp(name, entry)) {
                if (selected_entry) goto done;
                selected_entry = at; selected = p[2];
            }
        }
        if (op == 56) function = 0;
        at += count;
    }
    if (!selected || ids[selected].opcode != 54) goto done;
    ids[selected].live = 1;
    /* Follow actual calls, never assume module/function ordering. */
    int changed;
    do {
        changed = 0; function = 0;
        for (size_t at = 5; at < words; at += code[at] >> 16) {
            const uint32_t *p = code + at;
            uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
            if (op == 54) function = p[2];
            if (function && ids[function].live && op == 57) {
                if (count < 4 || p[3] >= bound || ids[p[3]].opcode != 54) goto done;
                if (!ids[p[3]].live) { ids[p[3]].live = 1; changed = 1; }
            }
            if (op == 56) function = 0;
        }
    } while (changed);
    for (uint32_t id = 1; id < bound; ++id)
        if (ids[id].owner) ids[id].live = ids[ids[id].owner].live;
    /* The selected entry's explicit interfaces must remain, even if unused. */
    const uint32_t *e = code + selected_entry;
    size_t interfaces = 3 + (strlen((const char *)(e + 3)) + 4) / 4;
    for (size_t i = interfaces; i < e[0] >> 16; ++i) {
        if (e[i] >= bound) goto done;
        ids[e[i]].live = 1;
    }
    function = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
        if (op == 54) function = p[2];
        if (function && ids[function].live)
            for (uint32_t i = 1; i < count; ++i)
                if (p[i] < bound && ids[p[i]].opcode == 59 && !ids[p[i]].owner) ids[p[i]].live = 1;
        if (op == 56) function = 0;
    }
    /* Initializers and ID decorations may keep additional globals alive. */
    do {
        changed = 0; function = 0;
        for (size_t at = 5; at < words; at += code[at] >> 16) {
            const uint32_t *p = code + at;
            uint32_t op = p[0] & 0xffff, count = p[0] >> 16, first = count;
            if (op == 54) function = p[2];
            if (!function) {
                if (op == 59 && count > 4 && ids[p[2]].live) first = 4;
                if (op == 332 && count > 3 && p[1] < bound && ids[p[1]].live) first = 3;
                if (op == 71 && count > 3 && p[1] < bound && ids[p[1]].live && p[2] == 5634) first = 3;
                if (op == 52 || op == 12) first = 3;
                for (uint32_t i = first; i < count; ++i)
                    if (p[i] < bound && ids[p[i]].opcode == 59 && !ids[p[i]].owner && !ids[p[i]].live) {
                        ids[p[i]].live = 1; changed = 1;
                    }
            }
            if (op == 56) function = 0;
        }
    } while (changed);
    result = hybris_scaled_alloc(allocator, size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!result) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(result, code, 20);
    size_t out = 5;
    function = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
        if (op == 54) function = p[2];
        int keep = !function || ids[function].live;
        if (!function) {
            if (op == 15) keep = at == selected_entry;
            if (op == 16 || op == 331) { if (count < 3) goto done; keep = p[1] == selected; }
            if (op == 59) keep = ids[p[2]].live;
            if (op == 5 || op == 6 || op == 71 || op == 72 || op == 332 || op == 5632 || op == 5633) {
                if (count < 2 || p[1] >= bound) goto done;
                keep = ids[p[1]].live;
            }
            if (op == 74 || op == 75 || op == 12) {
                /* Avoid dangling grouped/debug references into removed code. */
                for (uint32_t i = 1; i < count; ++i)
                    if (p[i] < bound && ids[p[i]].opcode && !ids[p[i]].live) {
                        *reason = "grouped or debug references prevent SPIR-V entry extraction";
                        goto done;
                    }
            }
        }
        if (keep) { memcpy(result + out, p, count * 4); out += count; }
        if (op == 56) function = 0;
    }
    *output = result; *output_size = out * 4;
    result = NULL; *reason = NULL; status = VK_SUCCESS;
done:
    hybris_scaled_free(allocator, result);
    hybris_scaled_free(allocator, ids);
    return status;
}
