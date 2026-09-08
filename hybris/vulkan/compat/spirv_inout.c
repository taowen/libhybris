/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "spirv_inout.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int enabled;
static void configure(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_COMPAT_INOUT");
    enabled = value && !strcmp(value, "1");
}
int hybris_inout_enabled(void) { pthread_once(&once, configure); return enabled; }
int hybris_inout_pipeline(const VkGraphicsPipelineCreateInfo *info)
{
    if (!hybris_inout_enabled() || info->pNext || (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR))
        return 0;
    unsigned vertex = 0, fragment = 0;
    for (uint32_t i = 0; i < info->stageCount; ++i) {
        if (info->pStages[i].pNext) return 0;
        vertex += info->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT;
        fragment += info->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT;
        if (info->pStages[i].stage != VK_SHADER_STAGE_VERTEX_BIT &&
            info->pStages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT) return 0;
    }
    return vertex == 1 && fragment == 1;
}

struct id {
    uint32_t op, at, type, object, value, loc;
    unsigned has_loc, builtin, component, blocked;
};

static int parse(const uint32_t *code, size_t size, const VkAllocationCallbacks *allocator,
    struct id **out, uint32_t *bound_out, unsigned storage)
{
    *out = NULL;
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return 0;
    uint32_t bound = code[3];
    size_t words = size / 4;
    if (sizeof(struct id) > SIZE_MAX / bound) return 0;
    struct id *ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!ids) return 0;
    memset(ids, 0, bound * sizeof(*ids));
    unsigned function = 0, entries = 0;
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (!count || count > words - at) { hybris_scaled_free(allocator, ids); return 0; }
        if (op == 15) ++entries;
        if (op == 54) function = 1;
        if (op == 56) function = 0;
        if (op == 21 || op == 22 || op == 23 || op == 32) {
            if (count < 2 || !p[1] || p[1] >= bound) { hybris_scaled_free(allocator, ids); return 0; }
            struct id *id = &ids[p[1]];
            id->op = op; id->at = (uint32_t)at;
            if (op == 21) { if (count != 4) { hybris_scaled_free(allocator, ids); return 0; } id->value = p[2]; id->object = p[3]; }
            if (op == 22) { if (count != 3) { hybris_scaled_free(allocator, ids); return 0; } id->value = p[2]; }
            if (op == 23) { if (count != 4 || p[2] >= bound) { hybris_scaled_free(allocator, ids); return 0; } id->object = p[2]; id->value = p[3]; }
            if (op == 32) { if (count != 4 || p[3] >= bound) { hybris_scaled_free(allocator, ids); return 0; } id->value = p[2]; id->object = p[3]; }
        } else if (op == 59) {
            if (count != 4 || p[1] >= bound || !p[2] || p[2] >= bound) { hybris_scaled_free(allocator, ids); return 0; }
            struct id *id = &ids[p[2]];
            id->op = op; id->at = (uint32_t)at; id->type = p[1]; id->value = p[3];
            id->blocked = function || p[3] != storage;
        } else if (op == 71 && count == 4 && p[1] < bound) {
            if (p[2] == 30) { ids[p[1]].has_loc = 1; ids[p[1]].loc = p[3]; }
            if (p[2] == 11) ids[p[1]].builtin = 1;
            if (p[2] == 31) ids[p[1]].component = 1;
        }
        at += count;
    }
    if (entries != 1) { hybris_scaled_free(allocator, ids); return 0; }
    *out = ids;
    *bound_out = bound;
    return 1;
}

static uint32_t scalar_of(const struct id *ids, uint32_t bound, uint32_t pointer, uint32_t *count)
{
    if (pointer >= bound || ids[pointer].op != 32) return 0;
    uint32_t pointee = ids[pointer].object;
    if (pointee >= bound) return 0;
    if (ids[pointee].op == 22 || ids[pointee].op == 21) { *count = 1; return pointee; }
    if (ids[pointee].op == 23 && ids[pointee].value >= 2 && ids[pointee].value <= 4 &&
        ids[pointee].object < bound) {
        *count = ids[pointee].value;
        return ids[pointee].object;
    }
    return 0;
}

static uint32_t find_vector(const uint32_t *code, size_t words, uint32_t scalar, uint32_t count)
{
    for (size_t at = 5; at < words;) {
        uint32_t n = code[at] >> 16, op = code[at] & 65535;
        if (!n || n > words - at) return 0;
        if (op == 23 && n == 4 && code[at + 2] == scalar && code[at + 3] == count) return code[at + 1];
        if (op == 54) break;
        at += n;
    }
    return 0;
}

static uint32_t find_pointer(const uint32_t *code, size_t words, uint32_t storage, uint32_t pointee)
{
    for (size_t at = 5; at < words;) {
        uint32_t n = code[at] >> 16, op = code[at] & 65535;
        if (!n || n > words - at) return 0;
        if (op == 32 && n == 4 && code[at + 2] == storage && code[at + 3] == pointee) return code[at + 1];
        if (op == 54) break;
        at += n;
    }
    return 0;
}

VkResult hybris_spirv_inout(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size, unsigned *widened)
{
    *output = NULL; *output_size = 0; *widened = 0;
    struct id *vs_ids = NULL, *fs_ids = NULL;
    uint32_t vs_bound = 0, fs_bound = 0;
    if (!parse(vs, vs_size, allocator, &vs_ids, &vs_bound, 3) ||
        !parse(fs, fs_size, allocator, &fs_ids, &fs_bound, 1)) {
        hybris_scaled_free(allocator, vs_ids);
        hybris_scaled_free(allocator, fs_ids);
        return VK_SUCCESS;
    }
    struct patch {
        uint32_t variable, scalar, old_count, new_count;
    } patches[8];
    unsigned patch_count = 0;
    size_t fs_words = fs_size / 4;
    for (uint32_t id = 1; id < fs_bound && patch_count < 8; ++id) {
        if (fs_ids[id].op != 59 || fs_ids[id].blocked || !fs_ids[id].has_loc ||
            fs_ids[id].builtin || fs_ids[id].component) continue;
        uint32_t fs_count = 0;
        uint32_t fs_scalar = scalar_of(fs_ids, fs_bound, fs_ids[id].type, &fs_count);
        if (!fs_scalar) continue;
        for (uint32_t vid = 1; vid < vs_bound; ++vid) {
            if (vs_ids[vid].op != 59 || vs_ids[vid].blocked || !vs_ids[vid].has_loc ||
                vs_ids[vid].builtin || vs_ids[vid].component || vs_ids[vid].loc != fs_ids[id].loc)
                continue;
            uint32_t vs_count = 0;
            uint32_t vs_scalar = scalar_of(vs_ids, vs_bound, vs_ids[vid].type, &vs_count);
            if (!vs_scalar || vs_count <= fs_count || vs_count > 4) break;
            if (vs_ids[vs_scalar].op != fs_ids[fs_scalar].op ||
                vs_ids[vs_scalar].value != fs_ids[fs_scalar].value) break;
            if (vs_ids[vs_scalar].op == 21 && vs_ids[vs_scalar].object != fs_ids[fs_scalar].object) break;
            patches[patch_count++] = (struct patch){id, fs_scalar, fs_count, vs_count};
            break;
        }
    }
    hybris_scaled_free(allocator, vs_ids);
    if (!patch_count) { hybris_scaled_free(allocator, fs_ids); return VK_SUCCESS; }

    for (size_t at = 5; at < fs_words;) {
        const uint32_t *p = fs + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (!count || count > fs_words - at) { hybris_scaled_free(allocator, fs_ids); return VK_SUCCESS; }
        for (unsigned i = 0; i < patch_count; ++i) {
            uint32_t var = patches[i].variable;
            int mentioned = 0;
            for (uint32_t k = 1; k < count; ++k) mentioned |= p[k] == var;
            if (!mentioned) continue;
            if (op == 15 || op == 5 || op == 6 || op == 59 || op == 71) continue;
            if (op == 61 && count >= 4 && p[3] == var) continue;
            hybris_scaled_free(allocator, fs_ids);
            return VK_SUCCESS;
        }
        at += count;
    }

    uint32_t wide_vec[8] = {0}, wide_ptr[8] = {0};
    unsigned vec_new[8] = {0}, ptr_new[8] = {0};
    uint32_t next = fs_bound;
    unsigned new_types = 0;
    for (unsigned i = 0; i < patch_count; ++i) {
        wide_vec[i] = find_vector(fs, fs_words, patches[i].scalar, patches[i].new_count);
        if (!wide_vec[i]) { wide_vec[i] = next++; vec_new[i] = 1; new_types += 1; }
        wide_ptr[i] = find_pointer(fs, fs_words, 1, wide_vec[i]);
        if (!wide_ptr[i]) { wide_ptr[i] = next++; ptr_new[i] = 1; new_types += 1; }
    }
    unsigned loads = 0;
    for (size_t at = 5; at < fs_words; at += fs[at] >> 16) {
        if ((fs[at] & 65535) != 61) continue;
        for (unsigned i = 0; i < patch_count; ++i)
            if (fs[at + 3] == patches[i].variable) ++loads;
    }
    if (next > UINT32_MAX - loads) { hybris_scaled_free(allocator, fs_ids); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    uint32_t load_id = next;
    next += loads;

    size_t extra_words = (size_t)new_types * 4 + (size_t)loads * 8 + 8;
    if (fs_words > SIZE_MAX / 4 - extra_words) { hybris_scaled_free(allocator, fs_ids); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    uint32_t *result = hybris_scaled_alloc(allocator, (fs_words + extra_words) * 4, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!result) { hybris_scaled_free(allocator, fs_ids); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memcpy(result, fs, 20);
    result[3] = next;
    size_t out = 5;
    uint32_t alloc = load_id;
    unsigned made = 0;
    for (size_t at = 5; at < fs_words;) {
        const uint32_t *p = fs + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        unsigned patch = 8;
        if (op == 59 && count == 4)
            for (unsigned i = 0; i < patch_count; ++i)
                if (p[2] == patches[i].variable) patch = i;
        if (patch < 8) {
            result[out++] = p[0];
            result[out++] = wide_ptr[patch];
            result[out++] = p[2];
            result[out++] = p[3];
            ++made;
        } else if (op == 61 && count >= 4) {
            unsigned load_patch = 8;
            for (unsigned i = 0; i < patch_count; ++i)
                if (p[3] == patches[i].variable) load_patch = i;
            if (load_patch < 8) {
                uint32_t wide_load = alloc++;
                result[out++] = (4u << 16) | 61;
                result[out++] = wide_vec[load_patch];
                result[out++] = wide_load;
                result[out++] = p[3];
                uint32_t old_n = patches[load_patch].old_count;
                if (old_n == 1) {
                    result[out++] = (5u << 16) | 81;
                    result[out++] = p[1];
                    result[out++] = p[2];
                    result[out++] = wide_load;
                    result[out++] = 0;
                } else {
                    result[out++] = ((5u + old_n) << 16) | 79;
                    result[out++] = p[1];
                    result[out++] = p[2];
                    result[out++] = wide_load;
                    result[out++] = wide_load;
                    for (uint32_t c = 0; c < old_n; ++c) result[out++] = c;
                }
                at += count;
                continue;
            }
            memcpy(result + out, p, count * 4);
            out += count;
        } else {
            memcpy(result + out, p, count * 4);
            out += count;
        }
        if (count >= 2 && (op == 21 || op == 22 || op == 23)) {
            uint32_t defined = p[1];
            for (unsigned i = 0; i < patch_count; ++i) {
                if (vec_new[i] && defined == patches[i].scalar) {
                    result[out++] = (4u << 16) | 23;
                    result[out++] = wide_vec[i];
                    result[out++] = patches[i].scalar;
                    result[out++] = patches[i].new_count;
                    vec_new[i] = 0;
                    defined = wide_vec[i];
                }
                if (ptr_new[i] && defined == wide_vec[i]) {
                    result[out++] = (4u << 16) | 32;
                    result[out++] = wide_ptr[i];
                    result[out++] = 1;
                    result[out++] = wide_vec[i];
                    ptr_new[i] = 0;
                }
            }
        }
        at += count;
    }
    hybris_scaled_free(allocator, fs_ids);
    if (!made) { hybris_scaled_free(allocator, result); return VK_SUCCESS; }
    *output = result;
    *output_size = out * 4;
    *widened = made;
    return VK_SUCCESS;
}
