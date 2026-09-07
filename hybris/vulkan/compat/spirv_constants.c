/* SPDX-License-Identifier: Apache-2.0 */
#include "spirv_constants.h"
#include <string.h>

struct constant {
    const uint32_t *instruction;
    uint32_t spec_id, value;
    unsigned has_spec_id, state;
};
struct hybris_spirv_constants {
    uint32_t bound;
    const VkSpecializationInfo *specialization;
    struct constant ids[];
};
VkResult hybris_spirv_constants_create(const uint32_t *code, size_t size,
    const VkSpecializationInfo *specialization, const VkAllocationCallbacks *allocator,
    struct hybris_spirv_constants **output)
{
    *output = NULL;
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_ERROR_UNKNOWN;
    if (sizeof(struct constant) > (SIZE_MAX - sizeof(struct hybris_spirv_constants)) / code[3])
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    size_t bytes = sizeof(struct hybris_spirv_constants) + code[3] * sizeof(struct constant);
    struct hybris_spirv_constants *c = hybris_scaled_alloc(allocator, bytes, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(c, 0, bytes);
    c->bound = code[3]; c->specialization = specialization;
    for (size_t at = 5; at < size / 4;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (!count || count > size / 4 - at) goto unsupported;
        if (op == 20 || op == 21) {
            if (count != (op == 20 ? 2u : 4u) || p[1] >= c->bound) goto unsupported;
            c->ids[p[1]].instruction = p;
        } else if (op >= 41 && op <= 52) {
            if (count < 3 || p[1] >= c->bound || p[2] >= c->bound) goto unsupported;
            c->ids[p[2]].instruction = p;
        } else if (op == 71 && count >= 3 && p[2] == 1) {
            if (count != 4 || p[1] >= c->bound) goto unsupported;
            c->ids[p[1]].spec_id = p[3]; c->ids[p[1]].has_spec_id = 1;
        }
        at += count;
    }
    *output = c;
    return VK_SUCCESS;
unsupported:
    hybris_scaled_free(allocator, c);
    return VK_ERROR_UNKNOWN;
}
static int scalar_type(struct hybris_spirv_constants *c, uint32_t id)
{
    if (!id || id >= c->bound || !c->ids[id].instruction) return 0;
    const uint32_t *p = c->ids[id].instruction;
    return (p[0] & 0xffff) == 20 ? 1 : (p[0] & 0xffff) == 21 && p[2] == 32 ? 2 + !!p[3] : 0;
}
static int evaluate(struct hybris_spirv_constants *c, uint32_t id, unsigned depth, uint32_t *value)
{
    if (!id || id >= c->bound || depth > 64) return 0;
    struct constant *constant = &c->ids[id];
    const uint32_t *p = constant->instruction;
    if (!p || (p[0] >> 16) < 3 || !scalar_type(c, p[1])) return 0;
    if (constant->state == 2) { *value = constant->value; return 1; }
    if (constant->state) return 0;
    constant->state = 1;
    uint32_t op = p[0] & 0xffff, count = p[0] >> 16, result = 0;
    if ((op == 48 || op == 49 || op == 50) && constant->has_spec_id && c->specialization) {
        const VkSpecializationInfo *info = c->specialization;
        if (info->mapEntryCount && !info->pMapEntries) return 0;
        for (uint32_t i = 0; i < info->mapEntryCount; ++i) {
            const VkSpecializationMapEntry *map = &info->pMapEntries[i];
            if (map->constantID != constant->spec_id) continue;
            if (!info->pData || map->size != 4 || map->offset > info->dataSize ||
                map->size > info->dataSize - map->offset) return 0;
            memcpy(&result, (const char *)info->pData + map->offset, 4);
            if (op != 50) result = !!result;
            goto resolved;
        }
    }
    if (op == 41 || op == 48) result = 1;
    else if (op == 42 || op == 49 || op == 46) result = 0;
    else if (op == 43 || op == 50) { if (count != 4) return 0; result = p[3]; }
    else if (op == 52) {
        if (count < 5) return 0;
        uint32_t a, b = 0, d = 0;
        if (!evaluate(c, p[4], depth + 1, &a)) return 0;
        if (count > 5 && !evaluate(c, p[5], depth + 1, &b)) return 0;
        if (count > 6 && !evaluate(c, p[6], depth + 1, &d)) return 0;
        uint32_t operation = p[3];
        int unary = operation == 113 || operation == 114 || operation == 124 ||
                    operation == 126 || operation == 168 || operation == 200;
        if (count != (operation == 169 ? 7u : unary ? 5u : 6u)) return 0;
        int64_t sa = (int32_t)a, sb = (int32_t)b;
        switch (operation) {
        case 113: case 114: case 124: result = a; break; /* U/SConvert, Bitcast (32-bit only). */
        case 126: result = 0u - a; break;
        case 128: result = a + b; break;
        case 130: result = a - b; break;
        case 132: result = a * b; break;
        case 134: if (!b) return 0; result = a / b; break;
        case 135: if (!b || (sa == INT32_MIN && sb == -1)) return 0; result = (uint32_t)(sa / sb); break;
        case 137: if (!b) return 0; result = a % b; break;
        case 138: if (!b) return 0; result = (uint32_t)(sa % sb); break;
        case 139: {
            if (!b) return 0;
            int64_t remainder = sa % sb;
            if (remainder && ((remainder < 0) != (sb < 0))) remainder += sb;
            result = (uint32_t)remainder; break;
        }
        case 164: case 170: result = a == b; break;
        case 165: case 171: result = a != b; break;
        case 166: result = a || b; break;
        case 167: result = a && b; break;
        case 168: result = !a; break;
        case 169: result = a ? b : d; break;
        case 172: result = a > b; break;
        case 173: result = sa > sb; break;
        case 174: result = a >= b; break;
        case 175: result = sa >= sb; break;
        case 176: result = a < b; break;
        case 177: result = sa < sb; break;
        case 178: result = a <= b; break;
        case 179: result = sa <= sb; break;
        case 194: if (b >= 32) return 0; result = a >> b; break;
        case 195:
            if (b >= 32) return 0;
            result = a >> b;
            if (b && (a & 0x80000000u)) result |= UINT32_MAX << (32 - b);
            break;
        case 196: if (b >= 32) return 0; result = a << b; break;
        case 197: result = a | b; break;
        case 198: result = a ^ b; break;
        case 199: result = a & b; break;
        case 200: result = ~a; break;
        default: return 0;
        }
    } else return 0;
resolved:
    constant->value = result; constant->state = 2; *value = result;
    return 1;
}
int hybris_spirv_constant_u32(struct hybris_spirv_constants *c,
    uint32_t id, uint32_t *value, int *is_signed)
{
    if (!id || id >= c->bound || !c->ids[id].instruction) return 0;
    int type = scalar_type(c, c->ids[id].instruction[1]);
    if (type < 2) return 0;
    *is_signed = type == 3;
    return evaluate(c, id, 0, value);
}
void hybris_spirv_constants_destroy(struct hybris_spirv_constants *c,
    const VkAllocationCallbacks *allocator)
{
    hybris_scaled_free(allocator, c);
}
