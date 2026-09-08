/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "point_size.h"
#include "spirv_builtins.h"
#include "spirv_entry.h"
#include <pthread.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int enabled;
static void configure(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_COMPAT_POINT_SIZE");
    enabled = value && !strcmp(value, "1");
}
int hybris_point_size_enabled(void) { pthread_once(&once, configure); return enabled; }
int hybris_point_size_pipeline(const VkGraphicsPipelineCreateInfo *info)
{
    if (!hybris_point_size_enabled() || info->pNext ||
        (info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || !info->pInputAssemblyState ||
        info->pInputAssemblyState->pNext || info->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
        !info->pRasterizationState || info->pRasterizationState->pNext ||
        info->pRasterizationState->polygonMode == VK_POLYGON_MODE_POINT)
        return 0;
    unsigned vertex = 0;
    for (uint32_t i = 0; i < info->stageCount; ++i) {
        VkShaderStageFlagBits stage = info->pStages[i].stage;
        if (stage != VK_SHADER_STAGE_VERTEX_BIT && stage != VK_SHADER_STAGE_FRAGMENT_BIT) return 0;
        if (info->pStages[i].pNext) return 0;
        vertex += stage == VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (info->pDynamicState && info->pDynamicState->pNext) return 0;
    if (info->pDynamicState) for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; ++i)
        if (info->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY ||
            info->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_POLYGON_MODE_EXT) return 0;
    return vertex == 1;
}
struct point_id {
    uint32_t op, type, object, value, owner, builtin;
    unsigned blocked, point, stores;
};
static uint32_t output_structure(const struct point_id *ids, uint32_t bound, uint32_t variable)
{
    if (variable >= bound || ids[variable].op != 59 || ids[variable].type >= bound) return 0;
    uint32_t pointer = ids[variable].type;
    if (ids[pointer].op != 32 || ids[pointer].value != 3 || ids[pointer].object >= bound) return 0;
    uint32_t object = ids[pointer].object;
    return ids[object].op == 30 ? object : 0;
}
VkResult hybris_spirv_point_size(const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    unsigned *removed)
{
    *output = NULL; *output_size = 0; *removed = 0;
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_SUCCESS;
    uint32_t bound = code[3];
    size_t words = size / 4;
    if (sizeof(struct point_id) > SIZE_MAX / bound) return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct point_id *ids = hybris_scaled_alloc(allocator, bound * sizeof(*ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    uint32_t *stripped = NULL;
    if (!ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(ids, 0, bound * sizeof(*ids));
    VkResult result = VK_SUCCESS;
    unsigned function = 0;
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (!count || count > words - at) goto done;
        if (op == 10 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633 ||
            (op == 17 && count == 2 && (p[1] == 53 || p[1] == 54))) goto done;
        if (op == 54) function = 1;
        if (op == 56) function = 0;
        if (op == 21 || op == 22 || op == 30 || op == 32) {
            if (count < 2 || !p[1] || p[1] >= bound) goto done;
            ids[p[1]].op = op;
            if (op == 21 || op == 22) { if (count < 3) goto done; ids[p[1]].value = p[2]; }
            if (op == 32) { if (count != 4 || p[3] >= bound) goto done; ids[p[1]].value = p[2]; ids[p[1]].object = p[3]; }
        } else if (op == 43 || op == 59 || op == 65 || op == 66) {
            if (count < 4 || p[1] >= bound || !p[2] || p[2] >= bound) goto done;
            struct point_id *id = &ids[p[2]];
            id->op = op; id->type = p[1]; id->value = p[3];
            if (op == 59) { id->blocked = function || p[3] != 3 || count != 4; id->owner = p[2]; }
            if (op == 43 && count != 4) id->blocked = 1;
            if (op == 65 || op == 66) { if (count != 5 || p[3] >= bound || p[4] >= bound) id->blocked = 1; else id->object = p[4]; }
        }
        at += count;
    }
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (op == 71 && count == 4 && p[1] < bound && p[2] == 11 && p[3] == 1) ids[p[1]].point = 1;
        if (op == 72 && count == 5 && p[1] < bound && p[3] == 11 && p[4] == 1) {
            if (ids[p[1]].builtin) goto done;
            ids[p[1]].builtin = p[2] + 1;
        }
    }
    for (uint32_t id = 1; id < bound; ++id) {
        if ((ids[id].op != 65 && ids[id].op != 66) || ids[id].blocked) continue;
        uint32_t owner = ids[id].value, object = output_structure(ids, bound, owner);
        uint32_t index = ids[id].object;
        if (object && ids[object].builtin && ids[index].op == 43 && !ids[index].blocked &&
            ids[index].type < bound && ids[ids[index].type].op == 21 && ids[ids[index].type].value == 32 &&
            ids[index].value == ids[object].builtin - 1) { ids[id].point = 1; ids[id].owner = owner; }
    }
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        if (op == 5 || op == 6 || op == 71 || op == 72 || op == 15) continue;
        for (uint32_t i = 1; i < count; ++i) {
            uint32_t id = p[i];
            if (id >= bound || hybris_spirv_literal_word(op, i)) continue;
            if (ids[id].op == 59 && !(op == 59 && i == 2) && output_structure(ids, bound, id)) {
                int direct = (op == 65 || op == 66) && i == 3 && count >= 5 && p[4] < bound &&
                    ids[p[4]].op == 43 && !ids[p[4]].blocked && ids[p[4]].type < bound &&
                    ids[ids[p[4]].type].op == 21 && ids[ids[p[4]].type].value == 32;
                if (!direct) ids[id].blocked = 1;
            }
            if (!ids[id].point || !ids[id].owner) continue;
            if ((op == 59 || op == 65 || op == 66) && i == 2) continue;
            if (op == 62 && count == 3 && i == 1 && p[2] < bound) {
                uint32_t value = p[2], type = ids[value].type;
                if (ids[value].op == 43 && !ids[value].blocked && type < bound &&
                    ids[type].op == 22 && ids[type].value == 32 && ids[value].value == 0x3f800000) {
                    ++ids[id].stores; continue;
                }
            }
            ids[ids[id].owner].blocked = 1;
        }
    }
    unsigned stores = 0;
    for (uint32_t id = 1; id < bound; ++id)
        if (ids[id].point && ids[id].owner && !ids[ids[id].owner].blocked) stores += ids[id].stores;
    if (!stores) goto done;
    stripped = hybris_scaled_alloc(allocator, size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!stripped) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(stripped, code, 20);
    size_t out = 5;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 65535;
        uint32_t target = op == 62 || op == 5 || op == 71 ? (count > 1 ? p[1] : 0) :
            op == 65 || op == 66 ? p[2] : 0;
        if (target < bound && ids[target].point && ids[target].owner && !ids[ids[target].owner].blocked &&
            (op == 62 || ids[target].op != 59)) continue;
        memcpy(stripped + out, p, count * 4); out += count;
    }
    result = hybris_spirv_unused_point_size(stripped, out * 4, allocator, output, output_size, removed);
    if (!*removed) { hybris_scaled_free(allocator, *output); *output = NULL; *output_size = 0; }
done:
    hybris_scaled_free(allocator, stripped);
    hybris_scaled_free(allocator, ids);
    return result;
}
