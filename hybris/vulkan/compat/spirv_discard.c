/* SPDX-License-Identifier: Apache-2.0 */
#include "vertex_stores.h"
#include "spirv_entry.h"
#include <string.h>

static const unsigned char result_positions[] = {
#include "spirv_results.inc"
};
struct discard_id { uint32_t type, offset; };
static VkResult scan(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, struct discard_id **out)
{
    *out = NULL;
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3])
        return VK_ERROR_UNKNOWN;
    uint32_t bound = code[3];
    if (sizeof(**out) > SIZE_MAX / bound) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct discard_id *ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids),
        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(ids, 0, bound * sizeof(*ids));
    for (size_t at = 5; at < size / 4;) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        if (!n || n > size / 4 - at || at > UINT32_MAX ||
            op >= sizeof(result_positions) || !result_positions[op]) goto invalid;
        unsigned result = result_positions[op] - 1;
        if (result) {
            if (n <= result || !p[result] || p[result] >= bound) goto invalid;
            ids[p[result]].offset = at;
            if (result == 2) {
                if (!p[1] || p[1] >= bound) goto invalid;
                ids[p[result]].type = p[1];
            }
        }
        at += n;
    }
    *out = ids;
    return VK_SUCCESS;
invalid:
    hybris_scaled_free(allocator, ids);
    return VK_ERROR_UNKNOWN;
}

VkResult hybris_spirv_storage_writes(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, int *writes)
{
    *writes = 0;
    struct discard_id *ids;
    VkResult result = scan(code, size, allocator, &ids);
    if (result != VK_SUCCESS) return result;
    for (size_t at = 5; at < size / 4; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        /* Image writes, integer/float atomics. AtomicLoad alone is read-only.
         * Conservatively retain extension atomics, even if unreachable. */
        if (op == 99 || (op >= 228 && op <= 242) || op == 318 || op == 319 ||
            op == 5614 || op == 5615 || op == 6035) { *writes = 1; break; }
        if (op != 62 && op != 63 && op != 64) continue;
        if (n < 3 || p[1] >= code[3] || !ids[p[1]].type) { result = VK_ERROR_UNKNOWN; break; }
        uint32_t type = ids[p[1]].type;
        const uint32_t *pointer = code + ids[type].offset;
        if (!ids[type].offset || (pointer[0] & 65535) != 32 || pointer[0] >> 16 != 4) {
            result = VK_ERROR_UNKNOWN; break;
        }
        if (pointer[2] == 2 || pointer[2] == 12 || pointer[2] == 5349) {
            *writes = 1; break;
        }
    }
    hybris_scaled_free(allocator, ids);
    return result;
}

VkResult hybris_spirv_discard(const uint32_t *source, size_t source_size, uint32_t model,
    const char *entry, const VkAllocationCallbacks *allocator, uint32_t **output,
    size_t *output_size, const char **reason)
{
    *output = NULL; *output_size = 0;
    *reason = "unsupported Position interface for vertex-store discard conversion";
    if (model != 0 && model != 2 && model != 3) return VK_ERROR_UNKNOWN;
    uint32_t *code = NULL;
    size_t size = 0;
    VkResult result = hybris_spirv_entry(source, source_size, model, entry, allocator,
        &code, &size, reason);
    if (result != VK_SUCCESS) return result;
    struct discard_id *ids = NULL;
    result = scan(code, size, allocator, &ids);
    if (result != VK_SUCCESS) goto done;
    uint32_t bound = code[3], next = bound, selected = 0;
    uint32_t position = 0, member = UINT32_MAX, vector = 0, scalar = 0, pointer = 0;
    uint32_t integer = 0, one = 0, two = 0, index = 0;
    unsigned stores = 0, function = 0;
    for (size_t at = 5; at < size / 4; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        if (op == 15 && n >= 4 && p[1] == model) selected = p[2];
        if (op == 16 && n >= 3 && p[2] == 11) {
            *reason = "discard conversion does not alter transform feedback outputs";
            result = VK_ERROR_UNKNOWN; goto done;
        }
        if (op == 54) function = p[2];
        if ((model == 3 && (op == 218 || op == 220)) ||
            (model != 3 && function == selected && op == 253)) ++stores;
        if (op == 56) function = 0;
        if (op != 59 || n < 4 || p[3] != 3) continue;
        uint32_t id = p[2], pt = p[1];
        const uint32_t *ptr = code + ids[pt].offset;
        if (!ids[pt].offset || (ptr[0] & 65535) != 32 || ptr[0] >> 16 != 4 || ptr[3] >= bound) {
            result = VK_ERROR_UNKNOWN; goto done;
        }
        uint32_t type = ptr[3];
        for (size_t d = 5; d < size / 4; d += code[d] >> 16) {
            const uint32_t *dec = code + d;
            uint32_t dn = dec[0] >> 16, dop = dec[0] & 65535, found = UINT32_MAX;
            if (dop == 71 && dn == 4 && dec[1] == id && dec[2] == 11 && dec[3] == 0) found = type;
            if (dop == 72 && dn == 5 && dec[1] == type && dec[3] == 11 && dec[4] == 0) {
                const uint32_t *structure = code + ids[type].offset;
                if (!ids[type].offset || (structure[0] & 65535) != 30 || dec[2] >= (structure[0] >> 16) - 2) {
                    result = VK_ERROR_UNKNOWN; goto done;
                }
                found = structure[2 + dec[2]];
                member = dec[2];
            }
            if (found != UINT32_MAX) {
                if (position) { result = VK_ERROR_UNKNOWN; goto done; }
                position = id; vector = found;
            }
        }
    }
    if (!selected || !stores || next > UINT32_MAX - 32 - stores) {
        result = VK_ERROR_UNKNOWN; goto done;
    }
    if (vector) {
        if (vector >= bound || !ids[vector].offset) { result = VK_ERROR_UNKNOWN; goto done; }
        const uint32_t *v = code + ids[vector].offset;
        if ((v[0] & 65535) != 23 || v[0] >> 16 != 4 || v[3] != 4 || v[2] >= bound) {
            result = VK_ERROR_UNKNOWN; goto done;
        }
        scalar = v[2];
        const uint32_t *f = code + ids[scalar].offset;
        if (!ids[scalar].offset || (f[0] & 65535) != 22 || f[0] >> 16 != 3 || f[2] != 32) {
            result = VK_ERROR_UNKNOWN; goto done;
        }
    }
    for (uint32_t id = 1; id < bound; ++id) if (ids[id].offset) {
        const uint32_t *p = code + ids[id].offset;
        uint32_t op = p[0] & 65535, n = p[0] >> 16;
        if (!scalar && op == 22 && n == 3 && p[2] == 32) scalar = id;
        if (!integer && op == 21 && n == 4 && p[2] == 32 && p[3] == 0) integer = id;
    }
    for (uint32_t id = 1; id < bound; ++id) if (ids[id].offset) {
        const uint32_t *p = code + ids[id].offset;
        uint32_t op = p[0] & 65535, n = p[0] >> 16;
        if (!vector && scalar && op == 23 && n == 4 && p[2] == scalar && p[3] == 4) vector = id;
        if (op == 43 && n == 4 && scalar && p[1] == scalar) {
            if (p[3] == 0x3f800000) one = id;
            if (p[3] == 0x40000000) two = id;
        }
        if (op == 43 && n == 4 && integer && p[1] == integer && p[3] == member) index = id;
    }
    for (uint32_t id = 1; id < bound; ++id) if (ids[id].offset) {
        const uint32_t *p = code + ids[id].offset;
        if (vector && (p[0] & 65535) == 32 && p[0] >> 16 == 4 && p[2] == 3 && p[3] == vector) pointer = id;
    }
    uint32_t declarations[64], declaration_count = 0;
#define DECL(op, ...) do { uint32_t data[] = {__VA_ARGS__}; unsigned n = sizeof(data) / 4; \
    declarations[declaration_count++] = ((n + 1) << 16) | (op); \
    memcpy(declarations + declaration_count, data, sizeof(data)); declaration_count += n; } while (0)
    if (!scalar) { scalar = next++; DECL(22, scalar, 32); }
    if (!vector) { vector = next++; DECL(23, vector, scalar, 4); }
    if (!pointer) { pointer = next++; DECL(32, pointer, 3, vector); }
    if (!one) { one = next++; DECL(43, scalar, one, 0x3f800000); }
    if (!two) { two = next++; DECL(43, scalar, two, 0x40000000); }
    uint32_t value = next++;
    DECL(44, vector, value, two, two, two, one);
    int added_position = !position;
    if (!position) { position = next++; DECL(59, pointer, position, 3); }
    if (member != UINT32_MAX) {
        if (!integer) { integer = next++; DECL(21, integer, 32, 0); }
        if (!index) { index = next++; DECL(43, integer, index, member); }
    }
#undef DECL
    size_t words = size / 4;
    if (words > SIZE_MAX / 4 - 128 || stores > (SIZE_MAX / 4 - words - 128) / 8) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done;
    }
    uint32_t *rewritten = hybris_scaled_alloc(allocator, (words + 128 + stores * 8) * 4,
        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!rewritten) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(rewritten, code, 20);
    size_t used = 5;
    unsigned decorated = !added_position, declared = 0;
    function = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        if (!decorated && op >= 19 && op <= 39) {
            uint32_t dec[] = {(4u << 16) | 71, position, 11, 0};
            memcpy(rewritten + used, dec, sizeof(dec)); used += 4; decorated = 1;
        }
        if (!declared && op == 54) {
            memcpy(rewritten + used, declarations, declaration_count * 4);
            used += declaration_count; declared = 1;
        }
        if (op == 54) function = p[2];
        if ((model == 3 && (op == 218 || op == 220)) ||
            (model != 3 && function == selected && op == 253)) {
            uint32_t target = position;
            if (member != UINT32_MAX) {
                target = next++;
                uint32_t access[] = {(5u << 16) | 65, pointer, target, position, index};
                memcpy(rewritten + used, access, sizeof(access)); used += 5;
            }
            uint32_t store[] = {(3u << 16) | 62, target, value};
            memcpy(rewritten + used, store, sizeof(store)); used += 3;
        }
        if (op == 56) function = 0;
        memcpy(rewritten + used, p, n * 4);
        if (added_position && op == 15 && p[2] == selected) {
            rewritten[used] += 1u << 16;
            rewritten[used + n] = position;
            ++used;
        }
        used += n;
    }
    rewritten[3] = next;
    *output = rewritten; *output_size = used * 4;
    result = VK_SUCCESS;
done:
    hybris_scaled_free(allocator, ids);
    hybris_scaled_free(allocator, code);
    return result;
}
