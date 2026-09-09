/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "layer.h"
#include "../compat/application_policy.h"
#include "../compat/scaled_vertex.h"
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <inttypes.h>

struct instance_state {
    VkInstance handle;
    void *key;
    PFN_vkGetInstanceProcAddr resolver;
    PFN_GetPhysicalDeviceProcAddr physical_resolver;
    unsigned policy;
    uint64_t generation;
    uint32_t api_version;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct instance_state *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct instance_state *instances;
static uint64_t next_generation;
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t trace_guard = PTHREAD_MUTEX_INITIALIZER;
static int trace_enabled;
static unsigned trace_count[2];

static void initialize_trace(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_TRACE");
    trace_enabled = value && !strcmp(value, "1");
}
void hybris_layer_trace_lifetime(const char *action, uint64_t generation,
    const void *handle, uint64_t instance_generation)
{
    pthread_once(&trace_once, initialize_trace);
    if (!trace_enabled) return;
    unsigned device = instance_generation != 0;
    const char *kind = device ? "DEVICE" : "INSTANCE";
    pthread_mutex_lock(&trace_guard);
    if (trace_count[device] < 256) {
        if (device)
            fprintf(stderr, "HYBRIS_VULKAN_DEVICE %s generation=%" PRIu64
                " handle=%p instance=%" PRIu64 "\n", action, generation, handle, instance_generation);
        else
            fprintf(stderr, "HYBRIS_VULKAN_INSTANCE %s generation=%" PRIu64
                " handle=%p\n", action, generation, handle);
    } else if (trace_count[device] == 256)
        fprintf(stderr, "HYBRIS_VULKAN_%s truncated\n", kind);
    if (trace_count[device] <= 256) ++trace_count[device];
    pthread_mutex_unlock(&trace_guard);
}

/* This is the loader's dispatch key, not a modification of the driver's
 * object layout. Physical devices use the instance dispatch key; command
 * buffers use the device key. No Vulkan objects are wrapped or synthesized. */
void *hybris_layer_dispatch_key(const void *handle)
{
    void *key = NULL;
    if (handle) memcpy(&key, handle, sizeof(key));
    return key;
}
static int instance_snapshot(const void *handle, struct instance_state *out)
{
    void *key = hybris_layer_dispatch_key(handle);
    int found = 0;
    pthread_mutex_lock(&guard);
    for (const struct instance_state *state = instances; state; state = state->next)
        if (state->key == key) { *out = *state; found = 1; break; }
    pthread_mutex_unlock(&guard);
    return found;
}
int hybris_layer_lookup_physical(VkPhysicalDevice physical, struct hybris_layer_physical *out)
{
    struct instance_state state;
    if (!instance_snapshot(physical, &state)) return 0;
    *out = (struct hybris_layer_physical){.instance = state.handle, .generation = state.generation,
        .api_version = state.api_version, .resolver = state.resolver, .application_policy = state.policy};
    return 1;
}

static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (void *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                   link->function != VK_LAYER_LINK_INFO))
        link = (void *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr resolver = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)resolver(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    struct instance_state *state = hybris_scaled_alloc(allocator, sizeof(*state),
        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *state = (struct instance_state){.resolver = resolver,
        .physical_resolver = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr,
        .policy = hybris_application_policy(info->pApplicationInfo),
        .custom_allocator = allocator != NULL};
    if (allocator) state->allocator = *allocator;
    state->api_version = info->pApplicationInfo && info->pApplicationInfo->apiVersion ?
        info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    pthread_mutex_lock(&guard);
    if (next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&guard);
        hybris_scaled_free(allocator, state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->generation = ++next_generation;
    pthread_mutex_unlock(&guard);
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, state); return result; }
    state->handle = *out;
    state->key = hybris_layer_dispatch_key(*out);
    pthread_mutex_lock(&guard);
    state->next = instances;
    instances = state;
    hybris_layer_trace_lifetime("create", state->generation, state->handle, 0);
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL destroy_instance(VkInstance handle, const VkAllocationCallbacks *allocator)
{
    struct instance_state *retired = NULL;
    pthread_mutex_lock(&guard);
    for (struct instance_state **slot = &instances; *slot; slot = &(*slot)->next)
        if ((*slot)->handle == handle) { retired = *slot; *slot = retired->next; break; }
    pthread_mutex_unlock(&guard);
    if (!retired) return;
    hybris_layer_trace_lifetime("destroy", retired->generation, handle, 0);
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
        retired->resolver(handle, "vkDestroyInstance");
    destroy(handle, allocator);
    hybris_scaled_free(retired->custom_allocator ? &retired->allocator : NULL, retired);
}

static PFN_vkVoidFunction VKAPI_CALL get_instance_proc(VkInstance instance, const char *name)
{
    if (!name) return NULL;
    /* These bootstrap addresses are also queried by the loader/layer chain
     * with a null instance. Application-visible scope is enforced by loader. */
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)get_instance_proc;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)hybris_layer_get_device_proc;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)hybris_layer_create_device;
    struct instance_state state;
    if (!instance || !instance_snapshot(instance, &state)) return NULL;
    PFN_vkVoidFunction next = state.resolver(instance, name);
    if (!next) return NULL;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)destroy_instance;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)hybris_layer_destroy_device;
    /* GIPA is instance-scoped, so a device's policy cannot select the address.
     * Every command wrapper checks the actual device again before changing it. */
    PFN_vkVoidFunction intercepted = hybris_layer_physical_proc(name);
    if (!intercepted) intercepted = hybris_layer_device_dispatch(name, state.policy != 0);
    return intercepted ? intercepted : next;
}
static PFN_vkVoidFunction VKAPI_CALL get_physical_proc(VkInstance instance, const char *name)
{
    struct instance_state state;
    if (!instance || !name || !instance_snapshot(instance, &state)) return NULL;
    PFN_vkVoidFunction next = state.physical_resolver ? state.physical_resolver(instance, name) : NULL;
    if (!next) return NULL;
    PFN_vkVoidFunction compat = hybris_layer_physical_proc(name);
    return compat ? compat : next;
}

__attribute__((visibility("default"))) VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version)
{
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = get_instance_proc;
    version->pfnGetDeviceProcAddr = hybris_layer_get_device_proc;
    version->pfnGetPhysicalDeviceProcAddr = get_physical_proc;
    return VK_SUCCESS;
}
