/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "shader_cleanup.h"
#include "spirv_builtins.h"
#include "shader_dispatch.h"
#include "../layer/layer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int enabled;
static _Atomic unsigned reports;
static void configure(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_COMPAT_UNUSED_BUILTINS");
    enabled = value && !strcmp(value, "1");
}
int hybris_shader_cleanup_enabled(void)
{
    pthread_once(&once, configure);
    return enabled;
}
VkResult VKAPI_CALL hybris_shader_cleanup_create(VkDevice device, const VkShaderModuleCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkShaderModule *module)
{
    struct hybris_layer_device context;
    if (!hybris_layer_lookup_device(device, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateShaderModule create = (PFN_vkCreateShaderModule)context.resolver(device, "vkCreateShaderModule");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    /* Extension-bearing creation requests keep their original semantics. */
    if (!info || info->pNext || !info->pCode) return create(device, info, allocator, module);
    uint32_t *code = NULL;
    size_t size = 0;
    unsigned removed = 0;
    VkResult result = hybris_spirv_unused_builtins(info->pCode, info->codeSize, allocator, &code, &size, &removed);
    if (result != VK_SUCCESS) return result;
    VkShaderModuleCreateInfo changed = *info;
    if (code) {
        changed.pCode = code; changed.codeSize = size;
        unsigned slot = atomic_load(&reports);
        while (slot < 129 && !atomic_compare_exchange_weak(&reports, &slot, slot + 1)) {}
        if (slot < 128) fprintf(stderr, "HYBRIS_UNUSED_BUILTINS removed=%u original=%zu converted=%zu\n", removed, info->codeSize, size);
        else if (slot == 128) fprintf(stderr, "HYBRIS_UNUSED_BUILTINS truncated\n");
    }
    result = create(device, &changed, allocator, module);
    hybris_scaled_free(allocator, code);
    return result;
}
PFN_vkVoidFunction hybris_shader_cleanup_proc(const char *name)
{
    /* Shader dispatch captures the original module, then calls cleanup. */
    return hybris_shader_cleanup_enabled() && !hybris_shader_enabled() && !strcmp(name, "vkCreateShaderModule") ?
        (PFN_vkVoidFunction)hybris_shader_cleanup_create : NULL;
}
