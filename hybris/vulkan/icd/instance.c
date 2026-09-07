/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "instance.h"
#include "device.h"
#include "wsi.h"
#include "swapchain.h"
#include "../compat/scaled_dispatch.h"
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

/* Driver handles keep their loader-owned dispatch header untouched. */
struct physical_state {
    VkPhysicalDevice handle;
    struct physical_state *next;
};

struct instance_state {
    VkInstance handle;
    uint64_t generation;
    PFN_vkGetInstanceProcAddr resolver;
    PFN_vkDestroyInstance destroy;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    int surface_enabled;
    int wayland_enabled;
    struct physical_state *physical;
    struct instance_state *next;
};
static pthread_mutex_t instance_guard = PTHREAD_MUTEX_INITIALIZER;
static struct instance_state *instances;
static uint64_t next_generation;
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static int trace_enabled;
static unsigned trace_count;

static void initialize_trace(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_ICD_INSTANCE_TRACE");
    trace_enabled = value && !strcmp(value, "1");
}

/* Caller holds the list guard, preserving create/destroy event order.
 * Debug output is bounded, disabled by default and contains no per-draw work. */
static void trace_instance(const char *action, const struct instance_state *state)
{
    if (!trace_enabled) return;
    if (trace_count < 256)
        fprintf(stderr, "HYBRIS_ICD_INSTANCE %s generation=%" PRIu64 " handle=%p\n",
                action, state->generation, (void *)state->handle);
    else if (trace_count == 256)
        fprintf(stderr, "HYBRIS_ICD_INSTANCE truncated\n");
    if (trace_count <= 256) ++trace_count;
}

static void free_state(struct instance_state *state)
{
    struct physical_state *physical = state->physical;
    while (physical) {
        struct physical_state *next = physical->next;
        if (state->custom_allocator)
            state->allocator.pfnFree(state->allocator.pUserData, physical);
        else
            free(physical);
        physical = next;
    }
    if (state->custom_allocator)
        state->allocator.pfnFree(state->allocator.pUserData, state);
    else
        free(state);
}

static void VKAPI_CALL destroy_instance(VkInstance instance,
                                        const VkAllocationCallbacks *allocator)
{
    if (!instance) return;
    pthread_mutex_lock(&instance_guard);
    struct instance_state **link = &instances;
    while (*link && (*link)->handle != instance) link = &(*link)->next;
    struct instance_state *state = *link;
    if (state) {
        *link = state->next;
        trace_instance("destroy", state);
    }
    pthread_mutex_unlock(&instance_guard);
    /* Vulkan requires external synchronization for destruction. Backend and
     * allocation callbacks run outside the guard; callbacks may use their own
     * locks but must not call Vulkan commands. */
    if (!state) return;
    hybris_icd_wsi_release_instance(instance);
    state->destroy(instance, allocator);
    free_state(state);
}

VkResult hybris_icd_create_instance(hwvulkan_device_t *hal,
    const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkInstance *instance)
{
    pthread_once(&trace_once, initialize_trace);
    struct instance_state *state = allocator
        ? allocator->pfnAllocation(allocator->pUserData, sizeof(*state),
                                  _Alignof(struct instance_state), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE)
        : malloc(sizeof(*state));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(state, 0, sizeof(*state));
    state->custom_allocator = allocator != NULL;
    if (allocator) state->allocator = *allocator;
    pthread_mutex_lock(&instance_guard);
    if (next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&instance_guard);
        free_state(state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->generation = ++next_generation;
    pthread_mutex_unlock(&instance_guard);
    VkInstanceCreateInfo filtered = *info;
    const char **wsi_names = NULL;
    VkResult prepared = hybris_icd_wsi_prepare_instance(info, &filtered, &wsi_names,
        &state->surface_enabled, &state->wayland_enabled);
    if (prepared != VK_SUCCESS) {
        free_state(state);
        return prepared;
    }
    VkResult result = hal->CreateInstance(&filtered, allocator, instance);
    hybris_icd_wsi_finish_instance(wsi_names);
    if (result != VK_SUCCESS) {
        free_state(state);
        return result;
    }
    state->handle = *instance;
    state->resolver = hal->GetInstanceProcAddr;
    state->destroy = (PFN_vkDestroyInstance)state->resolver(*instance, "vkDestroyInstance");
    pthread_mutex_lock(&instance_guard);
    state->next = instances;
    instances = state;
    trace_instance("create", state);
    pthread_mutex_unlock(&instance_guard);
    return result;
}

/* Instance destruction is externally synchronized against its child queries.
 * Never hold the registry lock while invoking driver or application callbacks. */
static struct instance_state *find_instance(VkInstance instance)
{
    pthread_mutex_lock(&instance_guard);
    struct instance_state *state = instances;
    while (state && state->handle != instance) state = state->next;
    pthread_mutex_unlock(&instance_guard);
    return state;
}

static VkResult remember_physical(struct instance_state *state, VkPhysicalDevice handle)
{
    pthread_mutex_lock(&instance_guard);
    struct physical_state *found = state->physical;
    while (found && found->handle != handle) found = found->next;
    pthread_mutex_unlock(&instance_guard);
    if (found) return VK_SUCCESS;
    struct physical_state *entry = state->custom_allocator
        ? state->allocator.pfnAllocation(state->allocator.pUserData, sizeof(*entry),
              _Alignof(struct physical_state), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE)
        : malloc(sizeof(*entry));
    if (!entry) return VK_ERROR_OUT_OF_HOST_MEMORY;
    entry->handle = handle;
    pthread_mutex_lock(&instance_guard);
    found = state->physical;
    while (found && found->handle != handle) found = found->next;
    if (!found) {
        entry->next = state->physical;
        state->physical = entry;
    }
    pthread_mutex_unlock(&instance_guard);
    if (found) {
        if (state->custom_allocator)
            state->allocator.pfnFree(state->allocator.pUserData, entry);
        else
            free(entry);
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL enumerate_physical(VkInstance instance, uint32_t *count,
                                               VkPhysicalDevice *physical)
{
    struct instance_state *state = find_instance(instance);
    PFN_vkEnumeratePhysicalDevices enumerate = state ?
        (PFN_vkEnumeratePhysicalDevices)state->resolver(instance, "vkEnumeratePhysicalDevices") : NULL;
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = enumerate(instance, count, physical);
    if (physical && (result == VK_SUCCESS || result == VK_INCOMPLETE))
        for (uint32_t i = 0; i < *count; ++i)
            if (remember_physical(state, physical[i]) != VK_SUCCESS) {
                *count = 0;
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
    return result;
}

static VkResult enumerate_groups(VkInstance instance, uint32_t *count,
                                 VkPhysicalDeviceGroupProperties *groups, const char *name)
{
    struct instance_state *state = find_instance(instance);
    PFN_vkEnumeratePhysicalDeviceGroups enumerate = state ?
        (PFN_vkEnumeratePhysicalDeviceGroups)state->resolver(instance, name) : NULL;
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = enumerate(instance, count, groups);
    if (groups && (result == VK_SUCCESS || result == VK_INCOMPLETE))
        for (uint32_t i = 0; i < *count; ++i)
            for (uint32_t j = 0; j < groups[i].physicalDeviceCount; ++j)
                if (remember_physical(state, groups[i].physicalDevices[j]) != VK_SUCCESS) {
                    *count = 0;
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                }
    return result;
}

static VkResult VKAPI_CALL enumerate_groups_core(VkInstance instance, uint32_t *count,
                                                 VkPhysicalDeviceGroupProperties *groups)
{
    return enumerate_groups(instance, count, groups, "vkEnumeratePhysicalDeviceGroups");
}

static VkResult VKAPI_CALL enumerate_groups_khr(VkInstance instance, uint32_t *count,
                                                VkPhysicalDeviceGroupProperties *groups)
{
    return enumerate_groups(instance, count, groups, "vkEnumeratePhysicalDeviceGroupsKHR");
}

static struct instance_state *find_physical(VkPhysicalDevice physical)
{
    pthread_mutex_lock(&instance_guard);
    struct instance_state *state = instances;
    for (; state; state = state->next) {
        struct physical_state *entry = state->physical;
        while (entry && entry->handle != physical) entry = entry->next;
        if (entry) break;
    }
    pthread_mutex_unlock(&instance_guard);
    return state;
}

int hybris_icd_lookup_instance_wsi(VkInstance instance, int *surface_enabled,
    int *wayland_enabled, uint64_t *generation)
{
    struct instance_state *state = find_instance(instance);
    if (!state) return 0;
    if (surface_enabled) *surface_enabled = state->surface_enabled;
    if (wayland_enabled) *wayland_enabled = state->wayland_enabled;
    if (generation) *generation = state->generation;
    return 1;
}

int hybris_icd_lookup_physical(VkPhysicalDevice physical,
    struct hybris_icd_physical *out)
{
    struct instance_state *state = find_physical(physical);
    if (!state || !out) return 0;
    out->instance = state->handle;
    out->generation = state->generation;
    out->resolver = state->resolver;
    return 1;
}

static void VKAPI_CALL format_properties(VkPhysicalDevice physical, VkFormat format,
                                         VkFormatProperties *properties)
{
    struct instance_state *state = find_physical(physical);
    if (!state) return;
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        state->resolver(state->handle, "vkGetPhysicalDeviceFormatProperties");
    query(physical, format, properties);
    hybris_scaled_format(query, physical, format, properties);
}
static void format_properties2(VkPhysicalDevice physical, VkFormat format,
                               VkFormatProperties2 *properties, const char *name)
{
    struct instance_state *state = find_physical(physical);
    if (!state) return;
    PFN_vkGetPhysicalDeviceFormatProperties2 query2 = (PFN_vkGetPhysicalDeviceFormatProperties2)
        state->resolver(state->handle, name);
    query2(physical, format, properties);
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        state->resolver(state->handle, "vkGetPhysicalDeviceFormatProperties");
    VkFormatFeatureFlags before = properties->formatProperties.bufferFeatures;
    hybris_scaled_format(query, physical, format, &properties->formatProperties);
    if (!(before & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) &&
        (properties->formatProperties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
        for (VkBaseOutStructure *next = properties->pNext; next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
                ((VkFormatProperties3 *)next)->bufferFeatures |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
}
static void VKAPI_CALL format_properties2_core(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *properties)
{ format_properties2(physical, format, properties, "vkGetPhysicalDeviceFormatProperties2"); }
static void VKAPI_CALL format_properties2_khr(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *properties)
{ format_properties2(physical, format, properties, "vkGetPhysicalDeviceFormatProperties2KHR"); }

static VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device)
{
    struct instance_state *state = find_physical(physical);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)state->resolver(state->handle, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr resolver = (PFN_vkGetDeviceProcAddr)
        state->resolver(state->handle, "vkGetDeviceProcAddr");
    if (!create || !resolver) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceFormatProperties query = (PFN_vkGetPhysicalDeviceFormatProperties)
        state->resolver(state->handle, "vkGetPhysicalDeviceFormatProperties");
    return hybris_icd_create_device(create, resolver, query, state->generation, physical, info, allocator, device);
}

static VkResult VKAPI_CALL enumerate_device_extensions(VkPhysicalDevice physical,
    const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
    return hybris_icd_enumerate_device_extensions(physical, layer, count, properties);
}

PFN_vkVoidFunction hybris_icd_instance_proc(VkInstance instance, const char *name)
{
    pthread_mutex_lock(&instance_guard);
    const struct instance_state *state = instances;
    while (state && state->handle != instance) state = state->next;
    PFN_vkGetInstanceProcAddr resolver = state ? state->resolver : NULL;
    int surface_enabled = state ? state->surface_enabled : 0;
    int wayland_enabled = state ? state->wayland_enabled : 0;
    pthread_mutex_unlock(&instance_guard);
    PFN_vkVoidFunction backend = resolver ? resolver(instance, name) : NULL;
    PFN_vkVoidFunction local_wsi = hybris_icd_wsi_proc(name, surface_enabled, wayland_enabled);
    if (local_wsi) return local_wsi;
    /* Device WSI entry points are adapter-owned. Enablement is checked on the
     * device object; GIPA may return the pointer before a device exists. */
    PFN_vkVoidFunction swapchain = hybris_icd_swapchain_proc(name, 1);
    if (swapchain) return swapchain;
    PFN_vkVoidFunction image = backend ? hybris_icd_swapchain_image_proc(name) : NULL;
    if (image) return image;
    /* Preserve the HAL's command scope and extension gating. */
    if (backend && !strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)destroy_instance;
    if (backend && !strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)enumerate_physical;
    if (backend && !strcmp(name, "vkEnumeratePhysicalDeviceGroups"))
        return (PFN_vkVoidFunction)enumerate_groups_core;
    if (backend && !strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
        return (PFN_vkVoidFunction)enumerate_groups_khr;
    if (backend && !strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)create_device;
    if (backend && !strcmp(name, "vkEnumerateDeviceExtensionProperties"))
        return (PFN_vkVoidFunction)enumerate_device_extensions;
    if (backend && !strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)hybris_icd_device_proc;
    if (backend && !strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)hybris_icd_destroy_device;
    if (backend && hybris_scaled_enabled()) {
        if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)format_properties;
        if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2")) return (PFN_vkVoidFunction)format_properties2_core;
        if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR")) return (PFN_vkVoidFunction)format_properties2_khr;
        PFN_vkVoidFunction compat = hybris_scaled_proc(name);
        if (compat) return compat;
    }
    return backend;
}
