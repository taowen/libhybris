/* SPDX-License-Identifier: Apache-2.0 */
#include "spirv_builtins.h"
#include "spirv_entry.h"
#include <string.h>

struct builtin_id {
    uint32_t op, at, type, object, value, builtin;
    uint64_t candidates, used;
    unsigned blocked, live, drop;
};
static unsigned popcount(uint64_t value)
{
    unsigned count = 0;
    while (value) { value &= value - 1; ++count; }
    return count;
}
static int clip_cull(uint32_t builtin) { return builtin == 3 || builtin == 4; }
static uint32_t structure(const struct builtin_id *ids, uint32_t bound, uint32_t var)
{
    if (var >= bound || ids[var].op != 59 || ids[var].type >= bound) return 0;
    uint32_t pointer = ids[var].type;
    if (ids[pointer].op != 32 || ids[pointer].value != 3 || ids[pointer].object >= bound) return 0;
    uint32_t object = ids[pointer].object;
    return ids[object].op == 30 ? object : 0;
}
static VkResult prune_outputs(const uint32_t *code, size_t size, unsigned builtin_mask,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *removed)
{
    *output = NULL; *output_size = 0; *removed = 0;
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_SUCCESS;
    uint32_t bound = code[3];
    size_t words = size / 4;
    if (sizeof(struct builtin_id) > SIZE_MAX / bound || words > (SIZE_MAX / 4 - 5) / 5 ||
        words > UINT32_MAX - bound) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct builtin_id *ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    uint32_t *result = NULL, *constants = NULL;
    VkResult status = VK_SUCCESS;
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(ids, 0, bound * sizeof(*ids));
    unsigned function = 0;
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (!count || count > words - at) goto done;
        /* Group annotations and extension/debug grammars need a separate
         * normalization step. Preserve the original rather than guess. */
        /* Transform feedback observes outputs outside ordinary shader uses. */
        if (op == 17 && count == 2 && (p[1] == 53 || p[1] == 54)) goto done;
        if (op == 10 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633) goto done;
        if (op == 54) function = 1;
        if (op == 56) function = 0;
        if (op == 21 || op == 30 || op == 32) {
            if (count < 2 || !p[1] || p[1] >= bound) goto done;
            struct builtin_id *id = &ids[p[1]];
            id->op = op; id->at = at;
            if (op == 21) { if (count != 4) goto done; id->value = p[2]; }
            if (op == 30 && count - 2 > 64) id->blocked = 1;
            if (op == 32) { if (count != 4 || p[3] >= bound) goto done; id->object = p[3]; id->value = p[2]; }
        } else if (op == 43 || op == 59) {
            if (count < 4 || p[1] >= bound || !p[2] || p[2] >= bound) goto done;
            struct builtin_id *id = &ids[p[2]];
            id->op = op; id->at = at; id->type = p[1]; id->value = p[3];
            if (op == 59) id->blocked = function || p[3] != 3 || count != 4;
        }
        at += count;
    }
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (op == 71 && count == 4 && p[1] < bound && p[2] == 11) ids[p[1]].builtin = p[3];
        if (op == 72 && count >= 4) {
            if (p[1] >= bound || ids[p[1]].op != 30 || p[2] >= (code[ids[p[1]].at] >> 16) - 2) goto done;
            if (p[3] == 11 && count == 5 && p[4] < 32 && (builtin_mask & (1u << p[4])) && p[2] < 64)
                ids[p[1]].candidates |= UINT64_C(1) << p[2];
        }
    }
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        uint32_t accessed = 0;
        if ((op == 65 || op == 66) && count >= 5 && p[3] < bound) {
            uint32_t object = structure(ids, bound, p[3]);
            if (object) {
                accessed = p[3];
                if (p[4] >= bound || ids[p[4]].op != 43 || ids[p[4]].type >= bound ||
                    ids[ids[p[4]].type].op != 21 || ids[ids[p[4]].type].value != 32 ||
                    (code[ids[p[4]].at] >> 16) != 4 || ids[p[4]].value >= 64)
                    ids[object].blocked = 1;
                else ids[object].used |= UINT64_C(1) << ids[p[4]].value;
            }
        }
        /* Names, decorations and entry interfaces are rewritten below. Other
         * references are conservative: ambiguous literals may prevent pruning. */
        if (op == 5 || op == 6 || op == 71 || op == 72 || op == 15) continue;
        for (uint32_t i = 1; i < count; ++i) {
            uint32_t id = p[i];
            if (id >= bound || hybris_spirv_literal_word(op, i)) continue;
            if (ids[id].op == 59 && !(op == 59 && i == 2)) {
                if (!(id == accessed && i == 3)) {
                    ids[id].live = 1;
                    uint32_t object = structure(ids, bound, id);
                    if (object) ids[object].blocked = 1;
                }
            }
            if (ids[id].op == 30 && !(op == 30 && i == 1) && !(op == 32 && i == 3 && p[2] == 3))
                ids[id].blocked = 1;
            if (ids[id].op == 32 && ids[id].object < bound && ids[ids[id].object].op == 30 &&
                !(op == 32 && i == 1) && !(op == 59 && i == 1 && p[3] == 3 && count == 4))
                ids[ids[id].object].blocked = 1;
        }
    }
    for (uint32_t id = 1; id < bound; ++id) {
        if (ids[id].op == 59 && !ids[id].blocked && !ids[id].live && ids[id].builtin < 32 && (builtin_mask & (1u << ids[id].builtin)))
            ids[id].drop = 1;
        if (ids[id].op == 30) {
            ids[id].candidates &= ~ids[id].used;
            if (ids[id].blocked) ids[id].candidates = 0;
            /* Keep nonempty interface blocks; direct empty blocks are uncommon
             * and removing their variable needs separate interface handling. */
            if (popcount(ids[id].candidates) == (code[ids[id].at] >> 16) - 2) ids[id].candidates = 0;
            *removed += popcount(ids[id].candidates);
        }
        *removed += ids[id].drop;
    }
    /* Remove each capability only after all corresponding declarations are
     * gone. Input builtins and active outputs keep their capability intact. */
    unsigned retained = 0, capability_changes = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (op == 71 && count == 4 && p[2] == 11 && clip_cull(p[3]) && p[1] < bound && !ids[p[1]].drop)
            retained |= 1u << (p[3] - 3);
        if (op == 72 && count == 5 && p[3] == 11 && clip_cull(p[4]) &&
            (p[2] >= 64 || !(ids[p[1]].candidates & (UINT64_C(1) << p[2])))) retained |= 1u << (p[4] - 3);
    }
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        if (p[0] == ((2u << 16) | 17) && (p[1] == 32 || p[1] == 33) && !(retained & (1u << (p[1] - 32)))) ++capability_changes;
    }
    if (!*removed && !capability_changes) goto done;
    result = hybris_scaled_alloc(allocator, (words * 5 + 5) * 4, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    constants = hybris_scaled_alloc(allocator, words * 4, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!result || !constants) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(result, code, 20);
    size_t out = 5, added = 0;
    uint32_t next = bound;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (op == 17 && count == 2 && (p[1] == 32 || p[1] == 33) && !(retained & (1u << (p[1] - 32)))) continue;
        if ((op == 5 || op == 71) && count > 1 && p[1] < bound && ids[p[1]].drop) continue;
        if (op == 59 && ids[p[2]].drop) continue;
        if ((op == 6 || op == 72) && count >= 3 && p[1] < bound && p[2] < 64) {
            uint64_t mask = ids[p[1]].candidates;
            if (mask & (UINT64_C(1) << p[2])) continue;
            memcpy(result + out, p, count * 4);
            result[out + 2] -= popcount(mask & ((UINT64_C(1) << p[2]) - 1));
            out += count; continue;
        }
        if (op == 30 && ids[p[1]].candidates) {
            size_t start = out;
            result[out++] = p[0]; result[out++] = p[1];
            for (uint32_t m = 0; m < count - 2; ++m)
                if (!(ids[p[1]].candidates & (UINT64_C(1) << m))) result[out++] = p[m + 2];
            result[start] = ((out - start) << 16) | op; continue;
        }
        if (op == 15 && count >= 4) {
            const char *end = memchr(p + 3, 0, (count - 3) * 4);
            if (!end) goto done;
            size_t interfaces = 3 + ((size_t)(end - (const char *)(p + 3)) + 4) / 4;
            size_t start = out;
            memcpy(result + out, p, interfaces * 4); out += interfaces;
            for (size_t i = interfaces; i < count; ++i)
                if (p[i] >= bound || !ids[p[i]].drop) result[out++] = p[i];
            result[start] = ((out - start) << 16) | op; continue;
        }
        memcpy(result + out, p, count * 4);
        if ((op == 65 || op == 66) && count >= 5) {
            uint32_t object = structure(ids, bound, p[3]);
            if (object && ids[object].candidates) {
                uint32_t member = ids[p[4]].value;
                unsigned delta = popcount(ids[object].candidates & ((UINT64_C(1) << member) - 1));
                if (delta) {
                    constants[added++] = (4u << 16) | 43;
                    constants[added++] = ids[p[4]].type;
                    constants[added++] = next;
                    constants[added++] = member - delta;
                    result[out + 4] = next++;
                }
            }
        }
        out += count;
    }
    /* New index constants precede functions and all rewritten access chains. */
    size_t insert = 5;
    while (insert < out && (result[insert] & 65535) != 54) insert += result[insert] >> 16;
    memmove(result + insert + added, result + insert, (out - insert) * 4);
    memcpy(result + insert, constants, added * 4);
    result[3] = next;
    *output = result; *output_size = (out + added) * 4; result = NULL;
done:
    if (!*output) *removed = 0;
    hybris_scaled_free(allocator, constants);
    hybris_scaled_free(allocator, result);
    hybris_scaled_free(allocator, ids);
    return status;
}

VkResult hybris_spirv_unused_builtins(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *removed)
{
    return prune_outputs(code, size, (1u << 3) | (1u << 4), allocator, output, output_size, removed);
}
VkResult hybris_spirv_unused_point_size(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *removed)
{
    return prune_outputs(code, size, 1u << 1, allocator, output, output_size, removed);
}
