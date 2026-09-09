/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "device.h"
#include "instance.h"
#include "swapchain.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* This table owns adapter metadata, never the loader's dispatch header.
 * Vulkan external synchronization still governs destruction and child use. */
struct device_state {
    VkDevice handle;
    uint64_t generation, instance_generation;
    PFN_vkGetDeviceProcAddr resolver;
    PFN_vkDestroyDevice destroy;
    VkPhysicalDevice physical;
    int swapchain_enabled;
    uint32_t queue_count;
    VkQueue *queues;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct device_state *next;
};
static pthread_mutex_t device_guard = PTHREAD_MUTEX_INITIALIZER;
static struct device_state *devices;
static uint64_t next_generation;
static void free_state(struct device_state *state)
{
    if (state->queues) {
        if (state->custom_allocator) state->allocator.pfnFree(state->allocator.pUserData, state->queues);
        else free(state->queues);
    }
    if (state->custom_allocator)
        state->allocator.pfnFree(state->allocator.pUserData, state);
    else
        free(state);
}

void VKAPI_CALL hybris_icd_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator)
{
    if (!device) return;
    hybris_icd_swapchain_release_device(device);
    pthread_mutex_lock(&device_guard);
    struct device_state **link = &devices;
    while (*link && (*link)->handle != device) link = &(*link)->next;
    struct device_state *state = *link;
    if (state) {
        *link = state->next;
    }
    pthread_mutex_unlock(&device_guard);
    if (!state) return;
    state->destroy(device, allocator);
    free_state(state);
}

VkResult hybris_icd_create_device(PFN_vkCreateDevice create, PFN_vkGetDeviceProcAddr resolver,
    uint64_t instance_generation, VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDevice *device)
{
    struct device_state *state = allocator
        ? allocator->pfnAllocation(allocator->pUserData, sizeof(*state),
                                  _Alignof(struct device_state), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
        : malloc(sizeof(*state));
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(state, 0, sizeof(*state));
    state->custom_allocator = allocator != NULL;
    if (allocator) state->allocator = *allocator;
    pthread_mutex_lock(&device_guard);
    if (next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&device_guard);
        free_state(state);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->generation = ++next_generation;
    pthread_mutex_unlock(&device_guard);
    VkDeviceCreateInfo filtered = *info;
    const char **wsi_names = NULL;
    VkResult prepared = hybris_icd_prepare_device(physical, NULL, VK_NULL_HANDLE, info,
        &filtered, &wsi_names, &state->swapchain_enabled);
    if (prepared != VK_SUCCESS) {
        free_state(state);
        return prepared;
    }
    VkResult result = create(physical, &filtered, allocator, device);
    hybris_icd_finish_device(wsi_names);
    if (result != VK_SUCCESS) {
        free_state(state);
        return result;
    }
    state->handle = *device;
    state->physical = physical;
    state->instance_generation = instance_generation;
    state->resolver = resolver;
    state->destroy = (PFN_vkDestroyDevice)resolver(*device, "vkDestroyDevice");
    // Queue handles identify the device even for a present with zero swapchains.
    // Allocate the whole registry at device creation, where OOM is reportable.
    uint64_t queue_count = 0;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
        queue_count += info->pQueueCreateInfos[i].queueCount;
    if (queue_count > UINT32_MAX || queue_count > SIZE_MAX / sizeof(VkQueue))
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
    else if (queue_count) {
        size_t size = (size_t)queue_count * sizeof(VkQueue);
        state->queues = allocator ? allocator->pfnAllocation(allocator->pUserData, size,
            _Alignof(VkQueue), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) : malloc(size);
        if (!state->queues) result = VK_ERROR_OUT_OF_HOST_MEMORY;
        else {
            PFN_vkGetDeviceQueue get = (PFN_vkGetDeviceQueue)resolver(*device, "vkGetDeviceQueue");
            PFN_vkGetDeviceQueue2 get2 = (PFN_vkGetDeviceQueue2)resolver(*device, "vkGetDeviceQueue2");
            for (uint32_t i = 0; i < info->queueCreateInfoCount && result == VK_SUCCESS; ++i) {
                const VkDeviceQueueCreateInfo *created = &info->pQueueCreateInfos[i];
                for (uint32_t j = 0; j < created->queueCount; ++j) {
                    VkQueue handle = VK_NULL_HANDLE;
                    if (!created->flags) get(*device, created->queueFamilyIndex, j, &handle);
                    else if (get2) {
                        VkDeviceQueueInfo2 qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                            .flags = created->flags, .queueFamilyIndex = created->queueFamilyIndex, .queueIndex = j};
                        get2(*device, &qi, &handle);
                    }
                    if (!handle) { result = VK_ERROR_INITIALIZATION_FAILED; break; }
                    state->queues[state->queue_count++] = handle;
                }
            }
        }
    }
    if (result != VK_SUCCESS) {
        state->destroy(*device, allocator);
        *device = VK_NULL_HANDLE;
        free_state(state);
        return result;
    }
    pthread_mutex_lock(&device_guard);
    state->next = devices;
    devices = state;
    pthread_mutex_unlock(&device_guard);
    return result;
}

PFN_vkVoidFunction VKAPI_CALL hybris_icd_device_proc(VkDevice device, const char *name)
{
    if (!device || !name) return NULL;
    pthread_mutex_lock(&device_guard);
    const struct device_state *state = devices;
    while (state && state->handle != device) state = state->next;
    PFN_vkGetDeviceProcAddr resolver = state ? state->resolver : NULL;
    int swapchain_enabled = state ? state->swapchain_enabled : 0;
    pthread_mutex_unlock(&device_guard);
    PFN_vkVoidFunction backend = resolver ? resolver(device, name) : NULL;
    PFN_vkVoidFunction local = hybris_icd_swapchain_proc(name, swapchain_enabled);
    if (local) return local;
    if (backend && swapchain_enabled) {
        PFN_vkVoidFunction image = hybris_icd_swapchain_image_proc(name);
        if (image) return image;
    }
    if (backend && !strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)hybris_icd_destroy_device;
    if (backend && !strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)hybris_icd_device_proc;
    return backend;
}

int hybris_icd_lookup_device(VkDevice device, struct hybris_icd_device *out)
{
    if (!device || !out) return 0;
    pthread_mutex_lock(&device_guard);
    const struct device_state *state = devices;
    while (state && state->handle != device) state = state->next;
    int found = state != NULL;
    if (found) {
        out->handle = state->handle;
        out->generation = state->generation;
        out->instance_generation = state->instance_generation;
        out->resolver = state->resolver;
        out->physical = state->physical;
        out->swapchain_enabled = state->swapchain_enabled;
    }
    pthread_mutex_unlock(&device_guard);
    return found;
}

int hybris_icd_lookup_queue(VkQueue queue, struct hybris_icd_device *out)
{
    VkDevice device = VK_NULL_HANDLE;
    pthread_mutex_lock(&device_guard);
    for (const struct device_state *state = devices; state && !device; state = state->next)
        for (uint32_t i = 0; i < state->queue_count; ++i)
            if (state->queues[i] == queue) { device = state->handle; break; }
    pthread_mutex_unlock(&device_guard);
    // Vulkan externally synchronizes device destruction against queue use.
    return hybris_icd_lookup_device(device, out);
}
