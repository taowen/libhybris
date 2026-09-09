/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "layer.h"
#include "device_features.h"
#include "../compat/application_policy.h"
#include "../compat/scaled_vertex.h"
#include "../compat/shader_dispatch.h"
#include "../compat/shader_policy.h"
#include "../compat/shader_cleanup.h"
#include "../compat/bc_policy.h"
#include "../compat/bc_context.h"
#include "../compat/clip_distance.h"
#include "../compat/memory_visibility.h"
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct device_state {
    struct hybris_layer_device device;
    void *key;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct device_state *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct device_state *devices;
static uint64_t next_generation;

static int lookup(const void *handle, struct hybris_layer_device *out)
{
    void *key = hybris_layer_dispatch_key(handle);
    int found = 0;
    pthread_mutex_lock(&guard);
    for (const struct device_state *state = devices; state; state = state->next)
        if (state->key == key) { *out = state->device; found = 1; break; }
    pthread_mutex_unlock(&guard);
    return found;
}
int hybris_layer_lookup_device(VkDevice device, struct hybris_layer_device *out)
{ return lookup(device, out); }
int hybris_layer_lookup_queue(VkQueue queue, struct hybris_layer_device *out)
{ return lookup(queue, out); }
int hybris_layer_device_allocator(VkDevice handle, VkAllocationCallbacks *out)
{
    int custom = 0;
    pthread_mutex_lock(&guard);
    for (const struct device_state *state = devices; state; state = state->next)
        if (state->device.handle == handle) {
            custom = state->custom_allocator;
            if (custom) *out = state->allocator;
            break;
        }
    pthread_mutex_unlock(&guard);
    return custom;
}

VkResult VKAPI_CALL hybris_layer_create_device(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *out)
{
    struct hybris_layer_physical instance;
    if (!hybris_layer_lookup_physical(physical, &instance)) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo *link = (void *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                   link->function != VK_LAYER_LINK_INFO)) link = (void *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_instance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr resolver = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_instance(instance.instance, "vkCreateDevice");
    if (!create || !resolver) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = hybris_bc_prepare_device(physical, info);
    if (result == VK_SUCCESS) result = hybris_shader_prepare_device(physical, info);
    if (result != VK_SUCCESS) return result;
    struct device_state *state = hybris_scaled_alloc(allocator, sizeof(*state), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *state = (struct device_state){.device = {.resolver = resolver, .physical = physical,
        .application_policy = hybris_application_device_policy(instance.application_policy, info),
        .track_commands = instance.application_policy != 0,
        .instance_generation = instance.generation}, .custom_allocator = allocator != NULL};
    if (allocator) state->allocator = *allocator;
    pthread_mutex_lock(&guard);
    if (next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&guard);
        hybris_scaled_free(allocator, state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->device.generation = ++next_generation;
    pthread_mutex_unlock(&guard);
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    struct hybris_device_features filtered;
    result = hybris_device_features_prepare(physical, info, allocator, &filtered);
    if (result == VK_SUCCESS) result = create(physical, &filtered.info, allocator, out);
    hybris_device_features_release(&filtered, allocator);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, state); return result; }
    state->device.handle = *out;
    state->key = hybris_layer_dispatch_key(*out);
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        instance.resolver(instance.instance, "vkGetPhysicalDeviceFormatProperties");
    result = hybris_shader_device_create(*out, physical, resolver, query, allocator);
    if (result == VK_SUCCESS) result = hybris_bc_attach_device(*out, physical, resolver, allocator);
    if (result != VK_SUCCESS) {
        hybris_shader_device_destroy(*out);
        ((PFN_vkDestroyDevice)resolver(*out, "vkDestroyDevice"))(*out, allocator);
        *out = VK_NULL_HANDLE;
        hybris_scaled_free(allocator, state);
        return result;
    }
    pthread_mutex_lock(&guard);
    state->next = devices;
    devices = state;
    hybris_layer_trace_lifetime("create", state->device.generation, *out, instance.generation);
    pthread_mutex_unlock(&guard);
    if (state->device.application_policy)
        fprintf(stderr, "HYBRIS_APPLICATION_POLICY flags=0x%x generation=%llu\n",
            state->device.application_policy, (unsigned long long)state->device.generation);
    return VK_SUCCESS;
}
void VKAPI_CALL hybris_layer_destroy_device(VkDevice handle, const VkAllocationCallbacks *allocator)
{
    /* Cleanup uses the live registry; remove the entry only after its children.
     * Device destruction is externally synchronized by Vulkan. */
    hybris_memory_visibility_release_device(handle);
    hybris_layer_commands_release_device(handle);
    hybris_bc_device_remove(handle);
    hybris_shader_device_destroy(handle);
    struct device_state *retired = NULL;
    pthread_mutex_lock(&guard);
    for (struct device_state **slot = &devices; *slot; slot = &(*slot)->next)
        if ((*slot)->device.handle == handle) { retired = *slot; *slot = retired->next; break; }
    pthread_mutex_unlock(&guard);
    if (!retired) return;
    hybris_layer_trace_lifetime("destroy", retired->device.generation, handle, retired->device.instance_generation);
    ((PFN_vkDestroyDevice)retired->device.resolver(handle, "vkDestroyDevice"))(handle, allocator);
    hybris_scaled_free(retired->custom_allocator ? &retired->allocator : NULL, retired);
}

PFN_vkVoidFunction hybris_layer_device_inner_proc(VkDevice handle, const char *name)
{
    struct hybris_layer_device device;
    if (!name || !hybris_layer_lookup_device(handle, &device) ||
        !hybris_shader_device_proc_allowed(handle, name)) return NULL;
    PFN_vkVoidFunction next = device.resolver(handle, name);
    if (!next) return NULL;
    PFN_vkVoidFunction compat = hybris_bc_proc(name);
    if (!compat) compat = hybris_shader_cleanup_proc(name);
    if (!compat) compat = hybris_shader_proc(name);
    return compat ? compat : next;
}
PFN_vkVoidFunction hybris_layer_device_dispatch(const char *name, int track_commands)
{
    PFN_vkVoidFunction compat = NULL;
    if (track_commands) {
        compat = hybris_memory_visibility_proc(name);
        if (!compat) compat = hybris_layer_commands_proc(name);
    }
    if (!compat) compat = hybris_bc_proc(name);
    if (!compat) compat = hybris_shader_cleanup_proc(name);
    if (!compat) compat = hybris_shader_proc(name);
    return compat;
}
PFN_vkVoidFunction VKAPI_CALL hybris_layer_get_device_proc(VkDevice handle, const char *name)
{
    struct hybris_layer_device device;
    if (!name || !hybris_layer_lookup_device(handle, &device) ||
        !hybris_shader_device_proc_allowed(handle, name)) return NULL;
    PFN_vkVoidFunction next = device.resolver(handle, name);
    if (!next) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)hybris_layer_get_device_proc;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)hybris_layer_destroy_device;
    PFN_vkVoidFunction compat = hybris_layer_device_dispatch(name, device.track_commands);
    return compat ? compat : next;
}
