/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "layer.h"
#include "../compat/application_policy.h"
#include "../compat/scaled_vertex.h"
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct instance_state {
    VkInstance handle;
    void *key;
    PFN_vkGetInstanceProcAddr resolver;
    PFN_GetPhysicalDeviceProcAddr physical_resolver;
    unsigned policy;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct instance_state *next;
};
struct device_state {
    struct hybris_layer_device device;
    void *key;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct device_state *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct instance_state *instances;
static struct device_state *devices;
static uint64_t next_generation;

/* This is the loader's dispatch key, not a modification of the driver's
 * object layout. Physical devices use the instance dispatch key; command
 * buffers use the device key. No Vulkan objects are wrapped or synthesized. */
static void *dispatch_key(const void *handle)
{
    void *key = NULL;
    if (handle) memcpy(&key, handle, sizeof(key));
    return key;
}
static int instance_snapshot(const void *handle, struct instance_state *out)
{
    void *key = dispatch_key(handle);
    int found = 0;
    pthread_mutex_lock(&guard);
    for (const struct instance_state *state = instances; state; state = state->next)
        if (state->key == key) { *out = *state; found = 1; break; }
    pthread_mutex_unlock(&guard);
    return found;
}
int hybris_layer_device(const void *handle, struct hybris_layer_device *out)
{
    void *key = dispatch_key(handle);
    int found = 0;
    pthread_mutex_lock(&guard);
    for (const struct device_state *state = devices; state; state = state->next)
        if (state->key == key) { *out = state->device; found = 1; break; }
    pthread_mutex_unlock(&guard);
    return found;
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
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, state); return result; }
    state->handle = *out;
    state->key = dispatch_key(*out);
    pthread_mutex_lock(&guard);
    state->next = instances;
    instances = state;
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
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
        retired->resolver(handle, "vkDestroyInstance");
    destroy(handle, allocator);
    hybris_scaled_free(retired->custom_allocator ? &retired->allocator : NULL, retired);
}

static VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDevice *out)
{
    struct instance_state instance;
    if (!instance_snapshot(physical, &instance)) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo *link = (void *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                   link->function != VK_LAYER_LINK_INFO))
        link = (void *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_instance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr resolver = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_instance(instance.handle, "vkCreateDevice");
    if (!create || !resolver) return VK_ERROR_INITIALIZATION_FAILED;
    struct device_state *state = hybris_scaled_alloc(allocator, sizeof(*state),
        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *state = (struct device_state){.device.resolver = resolver, .custom_allocator = allocator != NULL};
    if (allocator) state->allocator = *allocator;
    unsigned policy = hybris_application_device_policy(
        instance.policy & HYBRIS_APP_RENDERING_SEGMENTS, info);
    if (policy) {
        PFN_vkGetPhysicalDeviceProperties2 properties = (PFN_vkGetPhysicalDeviceProperties2)
            instance.resolver(instance.handle, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                            .pNext = &driver};
        if (properties) properties(physical, &props);
        /* The HAL ICD keeps its existing adapter. Applying both paths would
         * duplicate lowering. This layer currently enables only the observed
         * Turnip application profile; other drivers pass through unchanged. */
        if (driver.driverID != VK_DRIVER_ID_MESA_TURNIP) policy = 0;
    }
    state->device.policy = policy;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(physical, info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, state); return result; }
    state->device.handle = *out;
    state->key = dispatch_key(*out);
    pthread_mutex_lock(&guard);
    state->device.generation = ++next_generation;
    state->next = devices;
    devices = state;
    pthread_mutex_unlock(&guard);
    if (policy) fprintf(stderr, "hybris-layer: Turnip rendering segments enabled, device=%p generation=%llu\n",
        (void *)*out, (unsigned long long)state->device.generation);
    return result;
}
static void VKAPI_CALL destroy_device(VkDevice handle, const VkAllocationCallbacks *allocator)
{
    struct device_state *retired = NULL;
    pthread_mutex_lock(&guard);
    for (struct device_state **slot = &devices; *slot; slot = &(*slot)->next)
        if ((*slot)->device.handle == handle) { retired = *slot; *slot = retired->next; break; }
    pthread_mutex_unlock(&guard);
    if (!retired) return;
    hybris_layer_commands_release(&retired->device);
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)
        retired->device.resolver(handle, "vkDestroyDevice");
    destroy(handle, allocator);
    hybris_scaled_free(retired->custom_allocator ? &retired->allocator : NULL, retired);
}

static PFN_vkVoidFunction VKAPI_CALL get_device_proc(VkDevice device, const char *name)
{
    struct hybris_layer_device state;
    if (!name || !device || !hybris_layer_device(device, &state)) return NULL;
    PFN_vkVoidFunction next = state.resolver(device, name);
    if (!next) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)get_device_proc;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy_device;
    PFN_vkVoidFunction intercepted = state.policy ? hybris_layer_commands_proc(name) : NULL;
    return intercepted ? intercepted : next;
}
static PFN_vkVoidFunction VKAPI_CALL get_instance_proc(VkInstance instance, const char *name)
{
    if (!name) return NULL;
    /* These bootstrap addresses are also queried by the loader/layer chain
     * with a null instance. Application-visible scope is enforced by loader. */
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)get_instance_proc;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)get_device_proc;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)create_device;
    struct instance_state state;
    if (!instance || !instance_snapshot(instance, &state)) return NULL;
    PFN_vkVoidFunction next = state.resolver(instance, name);
    if (!next) return NULL;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)destroy_instance;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy_device;
    /* GIPA is instance-scoped, so a device's policy cannot select the address.
     * Every command wrapper checks the actual device again before changing it. */
    PFN_vkVoidFunction intercepted = hybris_layer_commands_proc(name);
    return intercepted ? intercepted : next;
}
static PFN_vkVoidFunction VKAPI_CALL get_physical_proc(VkInstance instance, const char *name)
{
    struct instance_state state;
    if (!instance || !name || !instance_snapshot(instance, &state)) return NULL;
    return state.physical_resolver ? state.physical_resolver(instance, name) : NULL;
}

__attribute__((visibility("default"))) VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version)
{
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        version->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = get_instance_proc;
    version->pfnGetDeviceProcAddr = get_device_proc;
    version->pfnGetPhysicalDeviceProcAddr = get_physical_proc;
    return VK_SUCCESS;
}
