/* SPDX-License-Identifier: Apache-2.0 */
#include "clip_distance.h"
#include "spirv_builtins.h"
#include <string.h>

struct emit {
    uint32_t *w;
    size_t n, cap;
};
struct parse_id {
    uint32_t op, at, type, object, value;
    unsigned location, has_location, builtin;
};

static uint32_t alloc_id(uint32_t *bound)
{
    uint32_t id = *bound;
    *bound += 1;
    return id;
}

static int grow(struct emit *out, size_t need, const VkAllocationCallbacks *allocator)
{
    uint32_t *words;
    size_t cap = out->cap;
    if (out->n + need <= cap) return 1;
    cap = cap ? cap : 64;
    while (cap < out->n + need) {
        if (cap > (SIZE_MAX / 8)) return 0;
        cap *= 2;
    }
    words = hybris_scaled_alloc(allocator, cap * sizeof(uint32_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!words) return 0;
    if (out->w) memcpy(words, out->w, out->n * sizeof(uint32_t));
    hybris_scaled_free(allocator, out->w);
    out->w = words;
    out->cap = cap;
    return 1;
}

static int emit(struct emit *out, const uint32_t *words, uint32_t count, const VkAllocationCallbacks *allocator)
{
    if (!grow(out, count, allocator)) return 0;
    memcpy(out->w + out->n, words, count * sizeof(uint32_t));
    out->n += count;
    return 1;
}

static int emit_ins(struct emit *out, uint32_t op, uint32_t count, const uint32_t *operands,
    const VkAllocationCallbacks *allocator)
{
    uint32_t header = (count << 16) | op;
    if (!grow(out, count, allocator)) return 0;
    out->w[out->n++] = header;
    if (count > 1) memcpy(out->w + out->n, operands, (count - 1) * sizeof(uint32_t));
    out->n += count - 1;
    return 1;
}

static uint32_t constant_value(const struct parse_id *ids, uint32_t bound, uint32_t id)
{
    if (!id || id >= bound || ids[id].op != 43) return 0xffffffffu;
    return ids[id].value;
}

static int parse_ids(const uint32_t *code, size_t words, struct parse_id *ids, uint32_t bound)
{
    unsigned function = 0;
    memset(ids, 0, bound * sizeof(*ids));
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (!count || count > words - at) return 0;
        if (op == 54) function = 1;
        if (op == 56) function = 0;
        if ((op == 20 || op == 21 || op == 22 || op == 28 || op == 30 || op == 32) && count >= 2 && p[1] && p[1] < bound) {
            ids[p[1]].op = op;
            ids[p[1]].at = (uint32_t)at;
            if (op == 21 && count == 4) ids[p[1]].value = p[3];
            if (op == 22 && count == 3) ids[p[1]].value = p[2];
            if (op == 28 && count == 4) { ids[p[1]].type = p[2]; ids[p[1]].object = p[3]; }
            if (op == 32 && count == 4) { ids[p[1]].value = p[2]; ids[p[1]].object = p[3]; }
        } else if (op == 43 && count == 4 && p[2] && p[2] < bound) {
            ids[p[2]].op = op;
            ids[p[2]].at = (uint32_t)at;
            ids[p[2]].type = p[1];
            ids[p[2]].value = p[3];
        } else if (op == 59 && count == 4 && p[2] && p[2] < bound) {
            ids[p[2]].op = op;
            ids[p[2]].at = (uint32_t)at;
            ids[p[2]].type = p[1];
            ids[p[2]].value = p[3];
            ids[p[2]].object = function;
        } else if (op == 71 && count == 4 && p[1] < bound) {
            if (p[2] == 30) { ids[p[1]].has_location = 1; ids[p[1]].location = p[3]; }
            if (p[2] == 11) ids[p[1]].builtin = p[3] + 1;
        }
        at += count;
    }
    return 1;
}

static uint32_t array_length(const struct parse_id *ids, uint32_t bound, uint32_t array)
{
    uint32_t len;
    if (!array || array >= bound || ids[array].op != 28) return 0;
    if (ids[array].type >= bound || ids[ids[array].type].op != 22 || ids[ids[array].type].value != 32)
        return 0;
    len = constant_value(ids, bound, ids[array].object);
    return (len >= 1 && len <= 8) ? len : 0;
}

static void mark_locations(const uint32_t *code, size_t words, unsigned used[64])
{
    for (size_t at = 5; at < words;) {
        uint32_t count = code[at] >> 16, op = code[at] & 65535;
        if (!count || count > words - at) return;
        if (op == 71 && count == 4 && code[at + 2] == 30 && code[at + 3] < 64)
            used[code[at + 3]] = 1;
        at += count;
    }
}

static int find_clip(const uint32_t *code, size_t words, const struct parse_id *ids, uint32_t bound,
    uint32_t *var, uint32_t *member, uint32_t *count, uint32_t *array)
{
    uint32_t found_var = 0, found_member = 0xffffffffu, found_array = 0, found_count = 0;
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        if (!n || n > words - at) return 0;
        if (op == 71 && n == 4 && p[1] < bound && p[2] == 11 && p[3] == 3) {
            if (ids[p[1]].op != 59 || ids[p[1]].value != 3) return 0;
            if (ids[p[1]].type >= bound || ids[ids[p[1]].type].op != 32) return 0;
            found_var = p[1];
            found_array = ids[ids[p[1]].type].object;
            found_count = array_length(ids, bound, found_array);
            found_member = 0xffffffffu;
        }
        if (op == 72 && n == 5 && p[1] < bound && p[3] == 11 && p[4] == 3) {
            uint32_t struct_id = p[1], index = p[2], type_at, members;
            if (ids[struct_id].op != 30) return 0;
            type_at = ids[struct_id].at;
            members = (code[type_at] >> 16) - 2;
            if (index >= members) return 0;
            found_array = code[type_at + 2 + index];
            found_count = array_length(ids, bound, found_array);
            found_member = index;
            found_var = 0;
            for (uint32_t id = 1; id < bound; ++id)
                if (ids[id].op == 59 && ids[id].value == 3 && ids[id].type < bound &&
                    ids[ids[id].type].op == 32 && ids[ids[id].type].object == struct_id &&
                    !ids[id].object)
                    found_var = id;
        }
        at += n;
    }
    if (!found_var || !found_count) return 0;
    *var = found_var;
    *member = found_member;
    *count = found_count;
    *array = found_array;
    return 1;
}

static uint32_t pick_location(unsigned used[64], uint32_t count)
{
    uint32_t start, i;
    for (start = 16; start + count <= 64; ++start) {
        for (i = 0; i < count; ++i)
            if (used[start + i]) break;
        if (i == count) return start;
    }
    return 0xffffffffu;
}

static uint32_t find_id_by_op(const struct parse_id *ids, uint32_t bound, uint32_t op, uint32_t value)
{
    uint32_t id;
    for (id = 1; id < bound; ++id)
        if (ids[id].op == op && ids[id].value == value) return id;
    return 0;
}

VkResult hybris_spirv_clip_plan(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size,
    uint32_t *location, uint32_t *count)
{
    struct parse_id *ids = NULL;
    uint32_t bound, var, member, array;
    uint32_t *cleaned = NULL;
    size_t cleaned_size = 0;
    unsigned removed = 0, used[64];
    *location = 0;
    *count = 0;
    if (!vs || vs_size < 20 || vs_size % 4 || vs[0] != 0x07230203 || !vs[3]) return VK_SUCCESS;
    /* A declaration alone must not add a fragment discard. In particular,
     * Blender's font shaders retain an unused gl_PerVertex ClipDistance
     * member: rewriting that member would create an unwritten varying. */
    VkResult result = hybris_spirv_unused_builtins(vs, vs_size, NULL,
        &cleaned, &cleaned_size, &removed);
    if (result != VK_SUCCESS) return result;
    if (cleaned) { vs = cleaned; vs_size = cleaned_size; }
    bound = vs[3];
    if (sizeof(*ids) > SIZE_MAX / bound) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    ids = hybris_scaled_alloc(NULL, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!ids) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    if (!parse_ids(vs, vs_size / 4, ids, bound) ||
        !find_clip(vs, vs_size / 4, ids, bound, &var, &member, count, &array)) {
        goto done;
    }
    memset(used, 0, sizeof(used));
    mark_locations(vs, vs_size / 4, used);
    if (fs && fs_size >= 20) mark_locations(fs, fs_size / 4, used);
    *location = pick_location(used, *count);
    if (*location == 0xffffffffu) *count = 0;
done:
    hybris_scaled_free(NULL, ids);
    hybris_scaled_free(NULL, cleaned);
    return result;
}

static int whole_struct_use(const uint32_t *code, size_t words, uint32_t var)
{
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        if (!n || n > words - at) return 1;
        if ((op == 61 && n >= 4 && p[3] == var) || (op == 62 && n >= 3 && p[1] == var))
            return 1;
        at += n;
    }
    return 0;
}

VkResult hybris_spirv_clip_vertex(const uint32_t *code, size_t size, uint32_t location,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *rewritten)
{
    struct parse_id *ids = NULL;
    struct emit out = {0};
    uint32_t bound, var, member, count, array, new_var, ptr, words_n;
    uint32_t *cleaned = NULL;
    size_t cleaned_size = 0;
    unsigned cleaned_removed = 0;
    size_t at;
    VkResult status = VK_SUCCESS;
    *output = NULL;
    *output_size = 0;
    *rewritten = 0;
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_SUCCESS;
    bound = code[3];
    words_n = (uint32_t)(size / 4);
    if (sizeof(*ids) > SIZE_MAX / bound) return VK_ERROR_OUT_OF_HOST_MEMORY;
    ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!parse_ids(code, words_n, ids, bound) ||
        !find_clip(code, words_n, ids, bound, &var, &member, &count, &array))
        goto done;
    if (whole_struct_use(code, words_n, var) && member != 0xffffffffu) goto done;
    ptr = alloc_id(&bound);
    new_var = alloc_id(&bound);
    if (!grow(&out, words_n + 32, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(out.w, code, 20);
    out.n = 5;
    out.w[3] = bound;
    {
        uint32_t dec[3] = {new_var, 30, location};
        int deco_done = 0, globals_done = 0;
    for (at = 5; at < words_n;) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        uint32_t copy[32];
        if (!n || n > words_n - at) { status = VK_ERROR_UNKNOWN; goto done; }
        if (op == 17 && n == 2 && (p[1] == 32 || p[1] == 7)) { at += n; continue; }
        if (op == 71 && n == 4 && p[2] == 11 && (p[3] == 3 || p[3] == 4)) { at += n; continue; }
        if (!deco_done && op != 17 && op != 11 && op != 14 && op != 15 && op != 16 &&
            op != 71 && op != 72 && op != 6 && op != 5 && op != 7 && op != 8 && op != 10 &&
            op != 3 && op != 4) {
            if (!emit_ins(&out, 71, 4, dec, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            deco_done = 1;
        }
        if (op == 15) {
            if (n + 1 > 32) { status = VK_ERROR_UNKNOWN; goto done; }
            memcpy(copy, p + 1, (n - 1) * sizeof(uint32_t));
            copy[n - 1] = new_var;
            if (!emit_ins(&out, 15, n + 1, copy, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            at += n;
            continue;
        }
        if (op == 54 && !globals_done) {
            uint32_t ins[3] = {ptr, 3, array};
            uint32_t var_ins[3] = {ptr, new_var, 3};
            if (!emit_ins(&out, 32, 4, ins, allocator) ||
                !emit_ins(&out, 59, 4, var_ins, allocator)) {
                status = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto done;
            }
            globals_done = 1;
        }
        if ((op == 65 || op == 66) && n >= 5 && p[3] == var && member != 0xffffffffu &&
            constant_value(ids, code[3], p[4]) == member) {
            uint32_t ops[8];
            if (n > 9) { status = VK_ERROR_UNKNOWN; goto done; }
            ops[0] = p[1];
            ops[1] = p[2];
            ops[2] = new_var;
            memcpy(ops + 3, p + 5, (n - 5) * sizeof(uint32_t));
            if (!emit_ins(&out, op, n - 1, ops, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            at += n;
            continue;
        }
        if ((op == 65 || op == 66) && n >= 4 && p[3] == var && member == 0xffffffffu) {
            uint32_t ops[8];
            if (n > 8) { status = VK_ERROR_UNKNOWN; goto done; }
            ops[0] = p[1];
            ops[1] = p[2];
            ops[2] = new_var;
            memcpy(ops + 3, p + 4, (n - 4) * sizeof(uint32_t));
            if (!emit_ins(&out, op, n, ops, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            at += n;
            continue;
        }
        if (!emit(&out, p, n, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        at += n;
    }
    }
    status = hybris_spirv_unused_builtins(out.w, out.n * 4, allocator, &cleaned, &cleaned_size, &cleaned_removed);
    if (status != VK_SUCCESS) goto done;
    if (cleaned) {
        hybris_scaled_free(allocator, out.w);
        out.w = NULL;
        *output = cleaned;
        *output_size = cleaned_size;
    } else {
        *output = out.w;
        *output_size = out.n * 4;
        out.w = NULL;
    }
    *rewritten = 1;
done:
    hybris_scaled_free(allocator, ids);
    hybris_scaled_free(allocator, out.w);
    if (status != VK_SUCCESS) {
        hybris_scaled_free(allocator, *output);
        *output = NULL;
        *output_size = 0;
        *rewritten = 0;
    }
    return status;
}

static uint32_t ensure_type(struct emit *prelude, struct parse_id *ids, uint32_t *bound,
    uint32_t op, uint32_t a, uint32_t b, const VkAllocationCallbacks *allocator)
{
    uint32_t id, existing;
    for (existing = 1; existing < *bound; ++existing)
        if (ids[existing].op == op && ids[existing].value == a && ids[existing].object == b &&
            (op != 32 || ids[existing].value == a))
            return existing;
    if (op == 22) {
        for (existing = 1; existing < *bound; ++existing)
            if (ids[existing].op == 22 && ids[existing].value == 32) return existing;
    }
    if (op == 21) {
        for (existing = 1; existing < *bound; ++existing)
            if (ids[existing].op == 21 && ids[existing].value == a) return existing;
    }
    if (op == 20) {
        existing = find_id_by_op(ids, *bound, 20, 0);
        if (existing) return existing;
    }
    id = alloc_id(bound);
    if (op == 20) {
        uint32_t ops[1] = {id};
        if (!emit_ins(prelude, 20, 2, ops, allocator)) return 0;
        ids[id].op = 20;
    } else if (op == 21) {
        uint32_t ops[3] = {id, 32, a};
        if (!emit_ins(prelude, 21, 4, ops, allocator)) return 0;
        ids[id].op = 21;
        ids[id].value = a;
    } else if (op == 22) {
        uint32_t ops[2] = {id, 32};
        if (!emit_ins(prelude, 22, 3, ops, allocator)) return 0;
        ids[id].op = 22;
        ids[id].value = 32;
    } else if (op == 28) {
        uint32_t ops[3] = {id, a, b};
        if (!emit_ins(prelude, 28, 4, ops, allocator)) return 0;
        ids[id].op = 28;
        ids[id].type = a;
        ids[id].object = b;
    } else if (op == 32) {
        uint32_t ops[3] = {id, a, b};
        if (!emit_ins(prelude, 32, 4, ops, allocator)) return 0;
        ids[id].op = 32;
        ids[id].value = a;
        ids[id].object = b;
    }
    return id;
}

static uint32_t ensure_constant(struct emit *prelude, struct parse_id *ids, uint32_t *bound,
    uint32_t type, uint32_t value, const VkAllocationCallbacks *allocator)
{
    uint32_t id, existing;
    for (existing = 1; existing < *bound; ++existing)
        if (ids[existing].op == 43 && ids[existing].type == type && ids[existing].value == value)
            return existing;
    id = alloc_id(bound);
    {
        uint32_t ops[3] = {type, id, value};
        if (!emit_ins(prelude, 43, 4, ops, allocator)) return 0;
    }
    ids[id].op = 43;
    ids[id].type = type;
    ids[id].value = value;
    return id;
}

VkResult hybris_spirv_clip_fragment(const uint32_t *code, size_t size, uint32_t location, uint32_t count,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *rewritten)
{
    struct parse_id *ids = NULL;
    struct emit out = {0}, prelude = {0};
    uint32_t bound, words_n, float_ty, int_ty, uint_ty, bool_ty, array_ty, ptr_arr, ptr_float;
    uint32_t zero, var, i, entry_fn = 0, first_label = 0, prelude_emitted = 0, id_cap;
    uint32_t index_const[8];
    uint32_t entry_label = 0, continuation_label = 0;
    size_t at;
    VkResult status = VK_SUCCESS;
    *output = NULL;
    *output_size = 0;
    *rewritten = 0;
    if (!count || count > 8 || size < 20 || size % 4 || code[0] != 0x07230203 || !code[3])
        return VK_SUCCESS;
    bound = code[3];
    id_cap = bound + 128;
    words_n = (uint32_t)(size / 4);
    if (sizeof(*ids) > SIZE_MAX / id_cap) return VK_ERROR_OUT_OF_HOST_MEMORY;
    ids = hybris_scaled_alloc(allocator, (bound + 128) * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(ids, 0, (bound + 128) * sizeof(*ids));
    if (!parse_ids(code, words_n, ids, bound)) { status = VK_ERROR_UNKNOWN; goto done; }
    float_ty = ensure_type(&prelude, ids, &bound, 22, 32, 0, allocator);
    int_ty = ensure_type(&prelude, ids, &bound, 21, 1, 0, allocator);
    uint_ty = ensure_type(&prelude, ids, &bound, 21, 0, 0, allocator);
    bool_ty = ensure_type(&prelude, ids, &bound, 20, 0, 0, allocator);
    if (!float_ty || !int_ty || !uint_ty || !bool_ty) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    {
        uint32_t len = ensure_constant(&prelude, ids, &bound, uint_ty, count, allocator);
        array_ty = 0;
        for (i = 1; i < bound; ++i)
            if (ids[i].op == 28 && ids[i].type == float_ty && ids[i].object == len) array_ty = i;
        if (!array_ty) array_ty = ensure_type(&prelude, ids, &bound, 28, float_ty, len, allocator);
    }
    ptr_arr = ensure_type(&prelude, ids, &bound, 32, 1, array_ty, allocator);
    ptr_float = ensure_type(&prelude, ids, &bound, 32, 1, float_ty, allocator);
    zero = ensure_constant(&prelude, ids, &bound, float_ty, 0, allocator);
    for (i = 0; i < count; ++i) {
        index_const[i] = ensure_constant(&prelude, ids, &bound, int_ty, i, allocator);
        if (!index_const[i]) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    }
    var = alloc_id(&bound);
    if (!ptr_arr || !ptr_float || !zero || !array_ty) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    {
        uint32_t var_ins[3] = {ptr_arr, var, 1};
        if (!emit_ins(&prelude, 59, 4, var_ins, allocator)) {
            status = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
    }
    if (!grow(&out, words_n + prelude.n + count * 16, allocator)) {
        status = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    memcpy(out.w, code, 20);
    out.n = 5;
    out.w[3] = bound;
    {
        uint32_t dec[3] = {var, 30, location};
        int deco_done = 0;
    for (at = 5; at < words_n;) {
        const uint32_t *p = code + at;
        uint32_t n = p[0] >> 16, op = p[0] & 65535;
        if (!n || n > words_n - at) { status = VK_ERROR_UNKNOWN; goto done; }
        if (!deco_done && op != 17 && op != 11 && op != 14 && op != 15 && op != 16 &&
            op != 71 && op != 72 && op != 6 && op != 5 && op != 7 && op != 8 && op != 10 &&
            op != 3 && op != 4) {
            if (!emit_ins(&out, 71, 4, dec, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            deco_done = 1;
        }
        if (op == 15) {
            uint32_t copy[48];
            if (n + 1 > 48) { status = VK_ERROR_UNKNOWN; goto done; }
            memcpy(copy, p + 1, (n - 1) * sizeof(uint32_t));
            copy[n - 1] = var;
            entry_fn = p[2];
            if (!emit_ins(&out, 15, n + 1, copy, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            at += n;
            continue;
        }
        if (op == 54) {
            if (!prelude_emitted) {
                if (!emit(&out, prelude.w, prelude.n, allocator)) {
                    status = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto done;
                }
                prelude_emitted = 1;
            }
            if (!emit(&out, p, n, allocator)) {
                status = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto done;
            }
            if (p[2] == entry_fn) first_label = 1;
            at += n;
            continue;
        }
        /* Function OpVariable must stay the first instructions in the first
         * block. Insert clip tests after those locals, not immediately after
         * the entry label. */
        if (op == 248 && first_label == 1 && p[1]) {
            if (!emit(&out, p, n, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            entry_label = p[1];
            first_label = 2;
            at += n;
            continue;
        }
        if (first_label == 2 && (op == 59 || op == 8 || op == 317)) {
            if (!emit(&out, p, n, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
            at += n;
            continue;
        }
        if (first_label == 2) {
            for (i = 0; i < count; ++i) {
                uint32_t chain, load, cmp, kill, cont;
                if (bound + 5 > id_cap) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
                chain = alloc_id(&bound);
                load = alloc_id(&bound);
                cmp = alloc_id(&bound);
                kill = alloc_id(&bound);
                cont = alloc_id(&bound);
                continuation_label = cont;
                uint32_t acc[4] = {ptr_float, chain, var, index_const[i]};
                uint32_t ld[3] = {float_ty, load, chain};
                uint32_t lt[4] = {bool_ty, cmp, load, zero};
                uint32_t merge[2] = {cont, 0};
                uint32_t br[3] = {cmp, kill, cont};
                if (!emit_ins(&out, 65, 5, acc, allocator) ||
                    !emit_ins(&out, 61, 4, ld, allocator) ||
                    !emit_ins(&out, 184, 5, lt, allocator) ||
                    !emit_ins(&out, 247, 3, merge, allocator) ||
                    !emit_ins(&out, 250, 4, br, allocator) ||
                    !emit_ins(&out, 248, 2, &kill, allocator) ||
                    !emit_ins(&out, 252, 1, NULL, allocator) ||
                    !emit_ins(&out, 248, 2, &cont, allocator)) {
                    status = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto done;
                }
            }
            out.w[3] = bound;
            first_label = 3;
        }
        size_t copied_at = out.n;
        if (!emit(&out, p, n, allocator)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        /* The original entry block's outgoing edges now leave the final
         * clip continuation. Retarget only OpPhi predecessor operands;
         * values and other label references retain their original IDs. */
        if (op == 245 && continuation_label) {
            for (uint32_t operand = 4; operand < n; operand += 2)
                if (out.w[copied_at + operand] == entry_label)
                    out.w[copied_at + operand] = continuation_label;
        }
        at += n;
    }
    }
    out.w[3] = bound;
    *output = out.w;
    *output_size = out.n * 4;
    *rewritten = 1;
    out.w = NULL;
done:
    hybris_scaled_free(allocator, ids);
    hybris_scaled_free(allocator, prelude.w);
    hybris_scaled_free(allocator, out.w);
    if (status != VK_SUCCESS) {
        hybris_scaled_free(allocator, *output);
        *output = NULL;
        *output_size = 0;
        *rewritten = 0;
    }
    return status;
}
