/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "layer.h"
#include "../compat/rendering_segments.h"
#include "../compat/scaled_vertex.h"
#include <pthread.h>
#include <string.h>

struct pool_state {
    VkCommandPool handle;
    VkDevice device;
    uint64_t generation;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct pool_state *next;
};
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
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct pool_state *pools;
static struct command_state *commands;

static PFN_vkVoidFunction inner(const struct hybris_layer_device *device, const char *name)
{ return device->resolver(device->handle, name); }
static void free_commands(struct command_state *list)
{
    while (list) {
        struct command_state *next = list->next;
        hybris_scaled_free(list->custom_allocator ? &list->allocator : NULL, list);
        list = next;
    }
}
static struct command_state *detach(const struct hybris_layer_device *device,
    VkCommandPool pool, VkCommandBuffer command, int whole_device)
{
    struct command_state *retired = NULL;
    for (struct command_state **slot = &commands; *slot;) {
        struct command_state *state = *slot;
        if (state->device == device->handle && state->generation == device->generation &&
            (whole_device || (state->pool == pool && (!command || state->handle == command)))) {
            *slot = state->next;
            state->next = retired;
            retired = state;
        } else slot = &state->next;
    }
    return retired;
}
static void release_pools(const struct hybris_layer_device *device, VkCommandPool pool, int all)
{
    struct pool_state *retired = NULL;
    pthread_mutex_lock(&guard);
    struct command_state *retired_commands = detach(device, pool, VK_NULL_HANDLE, all);
    for (struct pool_state **slot = &pools; *slot;) {
        struct pool_state *state = *slot;
        if (state->device == device->handle && state->generation == device->generation &&
            (all || state->handle == pool)) {
            *slot = state->next;
            state->next = retired;
            retired = state;
        } else slot = &state->next;
    }
    pthread_mutex_unlock(&guard);
    free_commands(retired_commands);
    while (retired) {
        struct pool_state *next = retired->next;
        hybris_scaled_free(retired->custom_allocator ? &retired->allocator : NULL, retired);
        retired = next;
    }
}
void hybris_layer_commands_release(const struct hybris_layer_device *device)
{ release_pools(device, VK_NULL_HANDLE, 1); }

static VkResult VKAPI_CALL create_pool(VkDevice handle, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *out)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)inner(&device, "vkCreateCommandPool");
    if (!device.policy) return create(handle, info, allocator, out);
    struct pool_state *state = hybris_scaled_alloc(allocator, sizeof(*state), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!state) { *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    *state = (struct pool_state){.device = handle, .generation = device.generation,
                                .custom_allocator = allocator != NULL};
    if (allocator) state->allocator = *allocator;
    VkResult result = create(handle, info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, state); return result; }
    state->handle = *out;
    pthread_mutex_lock(&guard);
    state->next = pools;
    pools = state;
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL destroy_pool(VkDevice handle, VkCommandPool pool,
    const VkAllocationCallbacks *allocator)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(handle, &device)) return;
    PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)inner(&device, "vkDestroyCommandPool");
    if (device.policy) release_pools(&device, pool, 0);
    destroy(handle, pool, allocator);
}
static VkResult VKAPI_CALL allocate_commands(VkDevice handle, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *out)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkAllocateCommandBuffers allocate = (PFN_vkAllocateCommandBuffers)inner(&device, "vkAllocateCommandBuffers");
    if (!device.policy) return allocate(handle, info, out);
    struct pool_state pool = {0};
    pthread_mutex_lock(&guard);
    for (const struct pool_state *state = pools; state; state = state->next)
        if (state->handle == info->commandPool && state->device == handle &&
            state->generation == device.generation) { pool = *state; break; }
    pthread_mutex_unlock(&guard);
    if (!pool.handle) return VK_ERROR_INITIALIZATION_FAILED;
    const VkAllocationCallbacks *allocator = pool.custom_allocator ? &pool.allocator : NULL;
    struct command_state *list = NULL;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) out[i] = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        struct command_state *state = hybris_scaled_alloc(allocator, sizeof(*state),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!state) { free_commands(list); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        *state = (struct command_state){.pool = pool.handle, .device = handle,
            .generation = device.generation, .allocator = pool.allocator,
            .custom_allocator = pool.custom_allocator, .next = list};
        list = state;
    }
    VkResult result = allocate(handle, info, out);
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
static void VKAPI_CALL free_command_buffers(VkDevice handle, VkCommandPool pool,
    uint32_t count, const VkCommandBuffer *buffers)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(handle, &device)) return;
    PFN_vkFreeCommandBuffers release = (PFN_vkFreeCommandBuffers)inner(&device, "vkFreeCommandBuffers");
    if (device.policy) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!buffers[i]) continue;
            pthread_mutex_lock(&guard);
            struct command_state *retired = detach(&device, pool, buffers[i], 0);
            pthread_mutex_unlock(&guard);
            free_commands(retired);
        }
    }
    release(handle, pool, count, buffers);
}
static void command_error(const struct hybris_layer_device *device, VkCommandBuffer command, VkResult error)
{
    pthread_mutex_lock(&guard);
    for (struct command_state *state = commands; state; state = state->next)
        if (state->handle == command && state->device == device->handle &&
            state->generation == device->generation) {
            if (error == VK_SUCCESS || state->error == VK_SUCCESS) state->error = error;
            break;
        }
    pthread_mutex_unlock(&guard);
}
static VkResult VKAPI_CALL reset_pool(VkDevice handle, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkResetCommandPool reset = (PFN_vkResetCommandPool)inner(&device, "vkResetCommandPool");
    VkResult result = reset(handle, pool, flags);
    if (result == VK_SUCCESS && device.policy) {
        pthread_mutex_lock(&guard);
        for (struct command_state *state = commands; state; state = state->next)
            if (state->device == handle && state->generation == device.generation && state->pool == pool)
                state->error = VK_SUCCESS;
        pthread_mutex_unlock(&guard);
    }
    return result;
}
static VkResult VKAPI_CALL begin_command(VkCommandBuffer command, const VkCommandBufferBeginInfo *info)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)inner(&device, "vkBeginCommandBuffer");
    VkResult result = begin(command, info);
    if (result == VK_SUCCESS && device.policy) command_error(&device, command, VK_SUCCESS);
    return result;
}
static VkResult VKAPI_CALL reset_command(VkCommandBuffer command, VkCommandBufferResetFlags flags)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkResetCommandBuffer reset = (PFN_vkResetCommandBuffer)inner(&device, "vkResetCommandBuffer");
    VkResult result = reset(command, flags);
    if (result == VK_SUCCESS && device.policy) command_error(&device, command, VK_SUCCESS);
    return result;
}
static VkResult VKAPI_CALL end_command(VkCommandBuffer command)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)inner(&device, "vkEndCommandBuffer");
    VkResult result = end(command);
    if (result == VK_SUCCESS && device.policy) {
        pthread_mutex_lock(&guard);
        for (const struct command_state *state = commands; state; state = state->next)
            if (state->handle == command && state->device == device.handle &&
                state->generation == device.generation) { result = state->error; break; }
        pthread_mutex_unlock(&guard);
    }
    return result;
}

static void begin_rendering(VkCommandBuffer command, const VkRenderingInfo *info, const char *name)
{
    struct hybris_layer_device device;
    if (!hybris_layer_device(command, &device)) return;
    PFN_vkCmdBeginRendering begin = (PFN_vkCmdBeginRendering)inner(&device, name);
    if (!device.policy) { begin(command, info); return; }
    VkAllocationCallbacks command_allocator;
    int custom = 0;
    pthread_mutex_lock(&guard);
    for (const struct command_state *state = commands; state; state = state->next)
        if (state->handle == command && state->device == device.handle &&
            state->generation == device.generation) {
            custom = state->custom_allocator;
            if (custom) command_allocator = state->allocator;
            break;
        }
    pthread_mutex_unlock(&guard);
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)inner(&device, "vkCmdPipelineBarrier");
    VkResult result = hybris_rendering_segments_begin(command, info, begin, barrier,
        custom ? &command_allocator : NULL);
    if (result != VK_SUCCESS) {
        command_error(&device, command, result);
        begin(command, info);
    }
}
static void VKAPI_CALL begin_core(VkCommandBuffer command, const VkRenderingInfo *info)
{ begin_rendering(command, info, "vkCmdBeginRendering"); }
static void VKAPI_CALL begin_khr(VkCommandBuffer command, const VkRenderingInfo *info)
{ begin_rendering(command, info, "vkCmdBeginRenderingKHR"); }

PFN_vkVoidFunction hybris_layer_commands_proc(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction function; } entries[] = {
#define ENTRY(api, function) {#api, (PFN_vkVoidFunction)function}
        ENTRY(vkCreateCommandPool, create_pool), ENTRY(vkDestroyCommandPool, destroy_pool),
        ENTRY(vkResetCommandPool, reset_pool), ENTRY(vkAllocateCommandBuffers, allocate_commands),
        ENTRY(vkFreeCommandBuffers, free_command_buffers), ENTRY(vkBeginCommandBuffer, begin_command),
        ENTRY(vkEndCommandBuffer, end_command), ENTRY(vkResetCommandBuffer, reset_command),
        ENTRY(vkCmdBeginRendering, begin_core), ENTRY(vkCmdBeginRenderingKHR, begin_khr),
#undef ENTRY
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(*entries); ++i)
        if (!strcmp(name, entries[i].name)) return entries[i].function;
    return NULL;
}
