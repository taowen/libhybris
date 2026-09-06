/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "instance.h"
#include <pthread.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

/* Driver handles keep their loader-owned dispatch header untouched. */
struct instance_state {
    VkInstance handle;
    uint64_t generation;
    PFN_vkGetInstanceProcAddr resolver;
    PFN_vkDestroyInstance destroy;
    VkAllocationCallbacks allocator;
    int custom_allocator;
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
     * allocation callbacks run outside the guard, so they may reenter. */
    if (!state) return;
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
    VkResult result = hal->CreateInstance(info, allocator, instance);
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

PFN_vkVoidFunction hybris_icd_instance_proc(VkInstance instance, const char *name)
{
    pthread_mutex_lock(&instance_guard);
    const struct instance_state *state = instances;
    while (state && state->handle != instance) state = state->next;
    PFN_vkGetInstanceProcAddr resolver = state ? state->resolver : NULL;
    pthread_mutex_unlock(&instance_guard);
    PFN_vkVoidFunction backend = resolver ? resolver(instance, name) : NULL;
    /* Preserve the HAL's command scope and extension gating. */
    if (backend && !strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)destroy_instance;
    return backend;
}
