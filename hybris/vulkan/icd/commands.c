/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "commands.h"
#include "../compat/application_policy.h"
#include "../compat/scaled_vertex.h"
#include <pthread.h>
#include <string.h>

/* Only adapter metadata is owned here. Driver command handles and their
 * dispatch headers remain untouched. Pool/device destruction is externally
 * synchronized by Vulkan; the guard also protects unrelated devices/pools. */
struct command_state {
    VkCommandBuffer handle;
    VkCommandPool pool;
    VkDevice device;
    uint64_t generation;
    VkResult error;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct command_state *next;
};
struct pool_state {
    VkCommandPool handle;
    VkDevice device;
    uint64_t generation;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct pool_state *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct command_state *commands;
static struct pool_state *pools;

static PFN_vkVoidFunction inner(const struct hybris_icd_device *device, const char *name)
{
    return hybris_icd_device_inner_proc(device->handle, name);
}
static void free_commands(struct command_state *list)
{
    while (list) {
        struct command_state *next = list->next;
        hybris_scaled_free(list->custom_allocator ? &list->allocator : NULL, list);
        list = next;
    }
}

static VkResult VKAPI_CALL create_pool(VkDevice handle, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *out)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    struct pool_state *pool = hybris_scaled_alloc(allocator, sizeof(*pool), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pool = (struct pool_state){.device = handle, .generation = device.generation,
        .custom_allocator = allocator != NULL};
    if (allocator) pool->allocator = *allocator;
    PFN_vkCreateCommandPool function = (PFN_vkCreateCommandPool)inner(&device, "vkCreateCommandPool");
    VkResult result = function(handle, info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, pool); return result; }
    pool->handle = *out;
    pthread_mutex_lock(&guard);
    pool->next = pools;
    pools = pool;
    pthread_mutex_unlock(&guard);
    return result;
}
int hybris_icd_command_device(VkCommandBuffer command, struct hybris_icd_device *device)
{
    VkDevice owner = VK_NULL_HANDLE;
    uint64_t generation = 0;
    pthread_mutex_lock(&guard);
    for (const struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) { owner = state->device; generation = state->generation; break; }
    pthread_mutex_unlock(&guard);
    return owner && hybris_icd_lookup_device(owner, device) && device->generation == generation;
}
int hybris_icd_command_allocator(VkCommandBuffer command, VkAllocationCallbacks *allocator)
{
    int custom = 0;
    pthread_mutex_lock(&guard);
    for (const struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) {
            custom = state->custom_allocator;
            if (custom) *allocator = state->allocator;
            break;
        }
    pthread_mutex_unlock(&guard);
    return custom;
}
void hybris_icd_command_error(VkCommandBuffer command, VkResult error)
{
    pthread_mutex_lock(&guard);
    for (struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) { state->error = error; break; }
    pthread_mutex_unlock(&guard);
}

static VkResult VKAPI_CALL allocate_commands(VkDevice handle, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *out)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    struct pool_state pool = {0};
    pthread_mutex_lock(&guard);
    for (const struct pool_state *state = pools; state; state = state->next)
        if (state->handle == info->commandPool && state->device == handle &&
            state->generation == device.generation) { pool = *state; break; }
    pthread_mutex_unlock(&guard);
    if (!pool.handle) return VK_ERROR_INITIALIZATION_FAILED;
    const VkAllocationCallbacks *allocator = pool.custom_allocator ? &pool.allocator : NULL;
    struct command_state *list = NULL;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        struct command_state *state = hybris_scaled_alloc(allocator, sizeof(*state),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!state) { free_commands(list); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        *state = (struct command_state){.pool = info->commandPool, .device = handle,
            .generation = device.generation, .allocator = pool.allocator,
            .custom_allocator = pool.custom_allocator, .next = list};
        list = state;
    }
    PFN_vkAllocateCommandBuffers function = (PFN_vkAllocateCommandBuffers)inner(&device, "vkAllocateCommandBuffers");
    VkResult result = function(handle, info, out);
    if (result != VK_SUCCESS) { free_commands(list); return result; }
    pthread_mutex_lock(&guard);
    for (uint32_t i = 0; list; ++i) {
        struct command_state *state = list;
        list = state->next;
        state->handle = out[i];
        state->next = commands;
        commands = state;
    }
    pthread_mutex_unlock(&guard);
    return result;
}

static struct command_state *detach(const struct hybris_icd_device *device, VkCommandPool pool,
    VkCommandBuffer handle, int whole_device)
{
    struct command_state *retired = NULL;
    pthread_mutex_lock(&guard);
    struct command_state **link = &commands;
    while (*link) {
        struct command_state *state = *link;
        if (state->device != device->handle || state->generation != device->generation ||
            (!whole_device && (state->pool != pool || (handle && state->handle != handle)))) {
            link = &state->next;
            continue;
        }
        *link = state->next;
        state->next = retired;
        retired = state;
    }
    pthread_mutex_unlock(&guard);
    return retired;
}
static void VKAPI_CALL release_commands(VkDevice handle, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer *list)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return;
    for (uint32_t i = 0; i < count; ++i)
        if (list[i]) free_commands(detach(&device, pool, list[i], 0));
    PFN_vkFreeCommandBuffers function = (PFN_vkFreeCommandBuffers)inner(&device, "vkFreeCommandBuffers");
    function(handle, pool, count, list);
}
static void VKAPI_CALL destroy_pool(VkDevice handle, VkCommandPool pool, const VkAllocationCallbacks *callbacks)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return;
    struct command_state *retired = detach(&device, pool, VK_NULL_HANDLE, 0);
    PFN_vkDestroyCommandPool function = (PFN_vkDestroyCommandPool)inner(&device, "vkDestroyCommandPool");
    function(handle, pool, callbacks);
    free_commands(retired);
    pthread_mutex_lock(&guard);
    struct pool_state **link = &pools;
    while (*link && ((*link)->handle != pool || (*link)->device != handle ||
        (*link)->generation != device.generation)) link = &(*link)->next;
    struct pool_state *state = *link;
    if (state) *link = state->next;
    pthread_mutex_unlock(&guard);
    if (state) hybris_scaled_free(state->custom_allocator ? &state->allocator : NULL, state);
}
void hybris_icd_commands_release_device(VkDevice handle)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return;
    free_commands(detach(&device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1));
    struct pool_state *retired = NULL;
    pthread_mutex_lock(&guard);
    struct pool_state **link = &pools;
    while (*link) {
        struct pool_state *state = *link;
        if (state->device != handle || state->generation != device.generation) {
            link = &state->next;
            continue;
        }
        *link = state->next;
        state->next = retired;
        retired = state;
    }
    pthread_mutex_unlock(&guard);
    while (retired) {
        struct pool_state *next = retired->next;
        hybris_scaled_free(retired->custom_allocator ? &retired->allocator : NULL, retired);
        retired = next;
    }
}

static VkResult VKAPI_CALL begin_command(VkCommandBuffer command, const VkCommandBufferBeginInfo *info)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandBufferBeginInfo begin = *info;
    VkCommandBufferInheritanceInfo inheritance;
    VkCommandBufferInheritanceRenderingInfo rendering;
    if ((device.application_policy & HYBRIS_APP_RENDERING_SEGMENTS) && info->pInheritanceInfo) {
        const VkBaseInStructure *node = info->pInheritanceInfo->pNext;
        if (node && node->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO) {
            rendering = *(const VkCommandBufferInheritanceRenderingInfo *)node;
            rendering.flags &= ~(VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
            inheritance = *info->pInheritanceInfo;
            inheritance.pNext = &rendering;
            begin.pInheritanceInfo = &inheritance;
        }
    }
    PFN_vkBeginCommandBuffer function = (PFN_vkBeginCommandBuffer)inner(&device, "vkBeginCommandBuffer");
    VkResult result = function(command, &begin);
    if (result == VK_SUCCESS) hybris_icd_command_error(command, VK_SUCCESS);
    return result;
}
static VkResult VKAPI_CALL end_command(VkCommandBuffer command)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEndCommandBuffer function = (PFN_vkEndCommandBuffer)inner(&device, "vkEndCommandBuffer");
    VkResult result = function(command);
    if (result != VK_SUCCESS) return result;
    pthread_mutex_lock(&guard);
    for (const struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) { result = state->error; break; }
    pthread_mutex_unlock(&guard);
    return result;
}

PFN_vkVoidFunction hybris_icd_commands_proc(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction function; } entries[] = {
        {"vkCreateCommandPool", (PFN_vkVoidFunction)create_pool},
        {"vkAllocateCommandBuffers", (PFN_vkVoidFunction)allocate_commands},
        {"vkFreeCommandBuffers", (PFN_vkVoidFunction)release_commands},
        {"vkDestroyCommandPool", (PFN_vkVoidFunction)destroy_pool},
        {"vkBeginCommandBuffer", (PFN_vkVoidFunction)begin_command},
        {"vkEndCommandBuffer", (PFN_vkVoidFunction)end_command},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(*entries); ++i)
        if (!strcmp(name, entries[i].name)) return entries[i].function;
    return NULL;
}
