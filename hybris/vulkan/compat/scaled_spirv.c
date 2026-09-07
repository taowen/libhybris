/* SPDX-License-Identifier: Apache-2.0 */
#include "scaled_vertex.h"
#include "spirv_entry.h"
#include "spirv_aggregate.h"
#include <stdint.h>
#include <string.h>

/* Stable opcode numbers from the Khronos SPIR-V grammar. This deliberately
 * handles a bounded interface subset; it is not a general SPIR-V optimizer.
 * Scaled formats are fetched as integers and converted at each original load,
 * preserving all floating-point result IDs and their uses. */
enum {
    OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_EXECUTION_MODE_ID = 331,
    OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
    OP_TYPE_POINTER = 32, OP_FUNCTION = 54, OP_VARIABLE = 59, OP_LOAD = 61,
    OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71,
    OP_COPY_OBJECT = 83, OP_CONVERT_S_TO_F = 111, OP_CONVERT_U_TO_F = 112
};
struct id_info {
    uint32_t opcode, element, width, storage, location;
    unsigned builtin, interface, sign;
};

static unsigned float_width(const struct id_info *ids, uint32_t bound, uint32_t type)
{
    if (type >= bound) return 0;
    if (ids[type].opcode == OP_TYPE_FLOAT) return ids[type].width == 32 ? 1 : 0;
    if (ids[type].opcode != OP_TYPE_VECTOR || ids[type].element >= bound) return 0;
    const struct id_info *scalar = &ids[ids[type].element];
    return scalar->opcode == OP_TYPE_FLOAT && scalar->width == 32 &&
           ids[type].width >= 2 && ids[type].width <= 4 ? ids[type].width : 0;
}

static VkResult convert_scaled(const uint32_t *code, size_t size, const char *entry,
    const struct hybris_scaled_attribute *attributes, uint32_t attribute_count,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason)
{
    *output = NULL;
    *output_size = 0;
    *reason = "unsupported or malformed SPIR-V interface";
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_ERROR_UNKNOWN;
    size_t words = size / 4;
    uint32_t bound = code[3];
    if (sizeof(struct id_info) > SIZE_MAX / bound || words > (SIZE_MAX / 4 - 128) / 2 ||
        bound > UINT32_MAX - 32 || words > UINT32_MAX - bound - 32)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct id_info *ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    uint32_t *result = NULL;
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(ids, 0, bound * sizeof(*ids));
    for (uint32_t i = 0; i < bound; ++i) ids[i].location = UINT32_MAX;
    VkResult status = VK_ERROR_UNKNOWN;
    uint32_t selected_function = 0;
    size_t selected_entry = 0;
    size_t first_type = 0;
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || count > words - at) goto done;
        const uint32_t *p = code + at;
        if (op == OP_ENTRY_POINT) {
            if (count < 4 || !p[2] || p[2] >= bound) goto done;
            const char *name = (const char *)(p + 3);
            const char *end = memchr(name, 0, (count - 3) * 4);
            if (!end) goto done;
            if (p[1] == 0 && !strcmp(name, entry)) {
                if (selected_entry) goto done;
                selected_entry = at;
                selected_function = p[2];
                size_t interfaces = 3 + ((size_t)(end - name) + 4) / 4;
                for (size_t i = interfaces; i < count; ++i) {
                    if (p[i] >= bound) goto done;
                    ids[p[i]].interface = 1;
                }
            }
        } else if (op == OP_DECORATE) {
            if (count < 3 || p[1] >= bound) goto done;
            if (p[2] == 30) { if (count != 4) goto done; ids[p[1]].location = p[3]; }
            if (p[2] == 11) ids[p[1]].builtin = 1;
        } else if (op == OP_TYPE_INT || op == OP_TYPE_FLOAT || op == OP_TYPE_VECTOR || op == OP_TYPE_POINTER) {
            if (count < 3 || p[1] >= bound) goto done;
            struct id_info *id = &ids[p[1]];
            id->opcode = op;
            if (op == OP_TYPE_FLOAT) id->width = p[2];
            else if (op == OP_TYPE_INT) { if (count != 4) goto done; id->width = p[2]; id->storage = p[3]; }
            else {
                if (count != 4 || p[3] >= bound) goto done;
                if (op == OP_TYPE_VECTOR) { id->element = p[2]; id->width = p[3]; }
                else { id->element = p[3]; id->storage = p[2]; }
            }
        } else if (op == OP_VARIABLE) {
            if (count < 4 || p[1] >= bound || p[2] >= bound) goto done;
            ids[p[2]].opcode = op;
            ids[p[2]].element = p[1];
            ids[p[2]].storage = p[3];
        }
        if (!first_type && op >= 19 && op <= 39) first_type = at;
        at += count;
    }
    if (!selected_entry || !first_type) {
        *reason = "scaled vertex conversion requires the named vertex entry point";
        goto done;
    }
    for (uint32_t i = 0; i < bound; ++i) {
        if (ids[i].opcode != OP_VARIABLE || ids[i].storage != 1 || !ids[i].interface) continue;
        if (ids[i].location == UINT32_MAX && !ids[i].builtin) {
            *reason = "scaled vertex interface blocks or grouped locations are not handled";
            goto done;
        }
        for (uint32_t j = 0; j < attribute_count; ++j) {
            if (ids[i].location != attributes[j].location) continue;
            uint32_t pointer = ids[i].element;
            if (ids[pointer].opcode != OP_TYPE_POINTER || ids[pointer].storage != 1 ||
                !float_width(ids, bound, ids[pointer].element)) goto done;
            ids[i].sign = attributes[j].is_signed ? 2 : 1;
        }
    }
    /* Pointer definitions dominate their uses for the supported operations.
     * Other Input-pointer-producing instructions are rejected, including phi,
     * select, function parameters and pointer-returning calls. */
    int in_function = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        const uint32_t *p = code + at;
        if (op == OP_FUNCTION) in_function = 1;
        if (!in_function) continue;
        if (op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_COPY_OBJECT) {
            if (count < 4 || p[1] >= bound || p[2] >= bound || p[3] >= bound) goto done;
            if (ids[p[3]].sign) {
                if (ids[p[1]].opcode != OP_TYPE_POINTER || ids[p[1]].storage != 1 ||
                    !float_width(ids, bound, ids[p[1]].element)) goto done;
                ids[p[2]].sign = ids[p[3]].sign;
            }
        } else if (count > 1 && !hybris_spirv_literal_word(op, 1) && p[1] < bound && ids[p[1]].opcode == OP_TYPE_POINTER && ids[p[1]].storage == 1) {
            *reason = "unsupported Input pointer producer in scaled vertex shader";
            goto done;
        }
        for (uint32_t i = 1; i < count; ++i) {
            if (hybris_spirv_literal_word(op, i) || p[i] >= bound || !ids[p[i]].sign) continue;
            if ((op == OP_LOAD && i == 3) ||
                ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_COPY_OBJECT) && (i == 2 || i == 3)))
                continue;
            *reason = "unsupported Input pointer use in scaled vertex shader";
            goto done;
        }
    }
    result = hybris_scaled_alloc(allocator, (words * 2 + 128) * 4, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!result) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(result, code, 20);
    size_t out = 5;
    uint32_t next_id = bound, integer[2][4] = {{0}}, pointer[2][4] = {{0}};
    for (unsigned sign = 0; sign < 2; ++sign) {
        for (uint32_t i = 1; i < bound; ++i)
            if (ids[i].opcode == OP_TYPE_INT && ids[i].width == 32 && ids[i].storage == sign)
                integer[sign][0] = i;
        if (!integer[sign][0]) integer[sign][0] = next_id++;
        for (unsigned n = 1; n < 4; ++n) {
            for (uint32_t i = 1; i < bound; ++i)
                if (ids[i].opcode == OP_TYPE_VECTOR && ids[i].width == n + 1 && ids[i].element == integer[sign][0])
                    integer[sign][n] = i;
            if (!integer[sign][n]) integer[sign][n] = next_id++;
        }
        for (unsigned n = 0; n < 4; ++n) {
            for (uint32_t i = 1; i < bound; ++i)
                if (ids[i].opcode == OP_TYPE_POINTER && ids[i].storage == 1 && ids[i].element == integer[sign][n])
                    pointer[sign][n] = i;
            if (!pointer[sign][n]) pointer[sign][n] = next_id++;
        }
    }
    in_function = 0;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        const uint32_t *p = code + at;
        /* A temporary module belongs to one pipeline stage. Keeping other
         * entry points could impose incompatible interface rules after a
         * shared Input variable becomes integer. Functions and declarations
         * remain available to the selected entry's call tree. */
        if (op == OP_ENTRY_POINT && at != selected_entry) continue;
        if (op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID) {
            if (count < 3) goto done;
            if (p[1] != selected_function) continue;
        }
        if (!in_function && op == OP_VARIABLE) continue;
        if (!in_function && op == OP_FUNCTION) {
            /* Reuse existing scalar/vector/pointer types to avoid illegal
             * duplicate declarations. Delay global variables until all types
             * (including replacement types) have been declared. */
            for (unsigned sign = 0; sign < 2; ++sign) {
                if (integer[sign][0] >= bound) {
                    result[out++] = (4u << 16) | OP_TYPE_INT;
                    result[out++] = integer[sign][0]; result[out++] = 32; result[out++] = sign;
                }
                for (unsigned n = 1; n < 4; ++n) if (integer[sign][n] >= bound) {
                    result[out++] = (4u << 16) | OP_TYPE_VECTOR;
                    result[out++] = integer[sign][n]; result[out++] = integer[sign][0]; result[out++] = n + 1;
                }
                for (unsigned n = 0; n < 4; ++n) if (pointer[sign][n] >= bound) {
                    result[out++] = (4u << 16) | OP_TYPE_POINTER;
                    result[out++] = pointer[sign][n]; result[out++] = 1; result[out++] = integer[sign][n];
                }
            }
            for (size_t declaration = 5; declaration < at; declaration += code[declaration] >> 16) {
                const uint32_t *v = code + declaration;
                if ((v[0] & 0xffff) != OP_VARIABLE) continue;
                uint32_t length = v[0] >> 16;
                memcpy(result + out, v, length * 4);
                if (ids[v[2]].sign) {
                    unsigned width = float_width(ids, bound, ids[v[1]].element);
                    if (!width) goto done;
                    result[out + 1] = pointer[ids[v[2]].sign - 1][width - 1];
                }
                out += length;
            }
            in_function = 1;
        }
        memcpy(result + out, p, count * 4);
        if ((op == OP_VARIABLE || op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_COPY_OBJECT) &&
            count >= 4 && p[2] < bound && ids[p[2]].sign) {
            unsigned width = float_width(ids, bound, ids[p[1]].element);
            if (!width) goto done;
            result[out + 1] = pointer[ids[p[2]].sign - 1][width - 1];
        } else if (op == OP_LOAD && count >= 4 && p[3] < bound && ids[p[3]].sign) {
            unsigned width = float_width(ids, bound, p[1]);
            unsigned sign = ids[p[3]].sign - 1;
            if (!width || p[2] >= bound) goto done;
            uint32_t loaded = next_id++;
            result[out + 1] = integer[sign][width - 1];
            result[out + 2] = loaded;
            out += count;
            result[out++] = (4u << 16) | (sign ? OP_CONVERT_S_TO_F : OP_CONVERT_U_TO_F);
            result[out++] = p[1]; result[out++] = p[2]; result[out++] = loaded;
            continue;
        }
        out += count;
    }
    result[3] = next_id;
    *output = result;
    *output_size = out * 4;
    result = NULL;
    *reason = NULL;
    status = VK_SUCCESS;
done:
    hybris_scaled_free(allocator, result);
    hybris_scaled_free(allocator, ids);
    return status;
}

VkResult hybris_scaled_spirv(const uint32_t *code, size_t size, const char *entry,
    const struct hybris_scaled_attribute *attributes, uint32_t attribute_count,
    const VkSpecializationInfo *specialization,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason)
{
    uint32_t *selected = NULL, *flat = NULL;
    size_t selected_size = 0, flat_size = 0;
    VkResult result = VK_SUCCESS;
    *output = NULL; *output_size = 0;
    if (hybris_spirv_multiple(code, size)) {
        result = hybris_spirv_entry(code, size, 0, entry, allocator, &selected, &selected_size, reason);
        if (result != VK_SUCCESS) goto done;
        code = selected; size = selected_size;
    }
    result = hybris_spirv_aggregate(code, size, entry, attributes, attribute_count,
                                    specialization, allocator, &flat, &flat_size, reason);
    if (result != VK_SUCCESS) goto done;
    if (flat) { code = flat; size = flat_size; }
    result = convert_scaled(code, size, entry, attributes, attribute_count, allocator, output, output_size, reason);
done:
    hybris_scaled_free(allocator, flat);
    hybris_scaled_free(allocator, selected);
    return result;
}
