/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pipeline ordering adapted from MaliBridge server/server_emu_shader.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors.
 * Modified integration: caller allocation, entry selection and explicit errors.
 */
#include "distance_lowering.h"
#include "spirv_entry.h"
#include "malibridge/spirv_clip.h"
#include "malibridge/spirv_pervertex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
static _Atomic unsigned reports;
static void note(const char *message, unsigned value)
{
    if (atomic_fetch_add(&reports, 1) < 32)
        fprintf(stderr, "HYBRIS_MALIBRIDGE_DISTANCE %s=%u\n", message, value);
}
static VkResult prepare(spv_mod *module, int wrap, const uint32_t *code, size_t size,
    int *changed, const char **reason)
{
    uint32_t *folded = NULL;
    size_t folded_size = 0;
    if (wrap) {
        const char *why = NULL;
        *changed = vkb_spv_wrap_pervertex(code, size, &folded, &folded_size, &why);
        if (!*changed && why) { *reason = why; return VK_ERROR_FEATURE_NOT_PRESENT; }
        if (*changed) { code = folded; size = folded_size; note("pervertex", 1); }
    }
    int parsed = spv_parse(module, code, size);
    free(folded);
    if (parsed < 0) { *reason = "MaliBridge SPIR-V parse failed"; return VK_ERROR_UNKNOWN; }
    return VK_SUCCESS;
}
static VkResult export_code(const spv_mod *module, const VkAllocationCallbacks *allocator,
    struct hybris_distance_code *out)
{
    if (module->err) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *words = NULL;
    size_t size = spv_write(module, &words);
    if (!size) return VK_ERROR_OUT_OF_HOST_MEMORY;
    out->code = hybris_scaled_alloc(allocator, size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (out->code) { memcpy(out->code, words, size); out->size = size; }
    free(words);
    return out->code ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
VkResult hybris_distance_module(unsigned what, int wrap, const uint32_t *code, size_t size,
    const VkAllocationCallbacks *allocator, struct hybris_distance_code *out, const char **reason)
{
    spv_mod module = {0};
    int changed = 0;
    VkResult result = prepare(&module, wrap, code, size, &changed, reason);
    if (result != VK_SUCCESS) goto done;
    if (what && vkb_spv_clip_needed(&module, what)) {
        vkb_spv_clip_info info;
        uint32_t base = (uint32_t)(vkb_spv_clip_max_location(&module) + 1);
        if (vkb_spv_clip_demote(&module, what, base, NULL, &info) < 0) {
            *reason = info.why ? info.why : "MaliBridge module demotion failed";
            result = module.err ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
            goto done;
        }
        changed = 1;
    }
    if (changed) result = export_code(&module, allocator, out);
done:
    spv_free(&module);
    return result;
}
VkResult hybris_distance_pipeline(unsigned what, int wrap, unsigned count,
    const struct hybris_distance_stage *stages, const VkAllocationCallbacks *allocator,
    struct hybris_distance_code *out, const char **reason)
{
    spv_mod modules[5] = {0};
    int index[5] = {-1,-1,-1,-1,-1}, changed[5] = {0};
    int needs = 0, location = -1;
    VkResult result = VK_SUCCESS;
    for (unsigned i = 0; i < count; ++i) {
        unsigned k = 0;
        while (k < 5 && (1u << k) != (unsigned)stages[i].stage) ++k;
        if (k == 5) { *reason = "MaliBridge lowering requires classic graphics stages"; result = VK_ERROR_FEATURE_NOT_PRESENT; goto done; }
        index[k] = (int)i;
        uint32_t *entry = NULL;
        size_t entry_size = 0;
        result = hybris_spirv_entry(stages[i].code, stages[i].size, k, stages[i].entry,
            allocator, &entry, &entry_size, reason);
        if (result != VK_SUCCESS) goto done;
        result = prepare(&modules[k], wrap, entry, entry_size, &changed[k], reason);
        hybris_scaled_free(allocator, entry);
        if (result != VK_SUCCESS) goto done;
        needs |= vkb_spv_clip_needed(&modules[k], what);
    }
    for (unsigned k = 0; needs && k < 5; ++k) {
        int high = index[k] >= 0 ? vkb_spv_clip_max_location(&modules[k]) : -1;
        if (high > location) location = high;
    }
    if (needs) {
        uint32_t base = (uint32_t)(location + 1), previous[3] = {0}, clip = 0;
        int first = 1, approximate = 0;
        for (unsigned k = 0; k < 5; ++k) {
            if (index[k] < 0) continue;
            spv_mod *module = &modules[k];
            vkb_spv_clip_info info = {0};
            int demote = vkb_spv_clip_needed(module, what);
            if (demote && vkb_spv_clip_demote(module, what, base, first ? NULL : previous, &info) < 0) {
                *reason = info.why ? info.why : "MaliBridge pipeline demotion failed";
                result = module->err ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
                goto done;
            }
            changed[k] |= demote;
            for (unsigned b = 0; b < 3; ++b) approximate |= info.in[b] && info.in[b] != previous[b];
            if (k < 4) {
                memcpy(previous, info.out, sizeof(previous));
                clip = info.out[0]; first = 0;
                if (info.out[1]) note("CullDistance_unapplied", info.out[1]);
            } else if (clip) {
                if (vkb_spv_clip_discard(module, base, clip, info.clip_in_var) < 0) {
                    *reason = "MaliBridge fragment discard failed";
                    result = module->err ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT;
                    goto done;
                }
                changed[k] = 1;
            }
        }
        if (clip && index[4] < 0) note("ClipDistance_without_fragment", clip);
        if (approximate) note("interface_count_mismatch", 1);
        note("pipeline_base", base);
    }
    for (unsigned k = 0; k < 5; ++k) if (index[k] >= 0 && changed[k]) {
        result = export_code(&modules[k], allocator, &out[index[k]]);
        if (result != VK_SUCCESS) goto done;
    }
done:
    for (unsigned k = 0; k < 5; ++k) spv_free(&modules[k]);
    return result;
}
