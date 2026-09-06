/* SPDX-License-Identifier: Apache-2.0 */
#include "render_dispatch.h"
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frontend metadata, without reading or modifying Android dispatch headers.
 * Vulkan's external synchronization requirements still apply to destruction.
 * No driver call or application allocation callback runs under the guard. */
struct allocation {
    VkAllocationCallbacks callbacks;
    int custom;
};
struct queue_state {
    uint32_t family, index;
    VkDeviceQueueCreateFlags flags;
    VkQueue handle;
};
struct device_state {
    VkDevice handle;
    struct allocation allocation;
    size_t queue_count;
    PFN_vkVoidFunction render[8];
    struct device_state *next;
    struct queue_state queues[];
};
struct pool_state {
    VkCommandPool handle;
    struct device_state *device;
    struct allocation allocation;
    struct pool_state *next;
};
struct buffer_state {
    VkCommandBuffer handle;
    struct pool_state *pool;
    struct buffer_state *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct device_state *devices;
static struct pool_state *pools;
static struct buffer_state *buffers;
static PFN_vkCreateDevice create_device;
static PFN_vkGetDeviceProcAddr device_proc;
static const char *render_names[] = {
    "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdPipelineBarrier2", "vkQueueSubmit2",
    "vkCmdBeginRenderingKHR", "vkCmdEndRenderingKHR", "vkCmdPipelineBarrier2KHR", "vkQueueSubmit2KHR"
};

static struct allocation allocation_for(const VkAllocationCallbacks *callbacks)
{
    struct allocation allocation = {0};
    if (callbacks) { allocation.callbacks = *callbacks; allocation.custom = 1; }
    return allocation;
}
static void *allocate(const struct allocation *allocation, size_t size,
                      VkSystemAllocationScope scope)
{
    void *memory = allocation->custom
        ? allocation->callbacks.pfnAllocation(allocation->callbacks.pUserData,
                                               size, _Alignof(max_align_t), scope)
        : malloc(size);
    if (memory) memset(memory, 0, size);
    return memory;
}
static void release(const struct allocation *allocation, void *memory)
{
    if (allocation->custom)
        allocation->callbacks.pfnFree(allocation->callbacks.pUserData, memory);
    else free(memory);
}
static struct device_state *find_device(VkDevice handle)
{
    for (struct device_state *state = devices; state; state = state->next)
        if (state->handle == handle) return state;
    return NULL;
}
static struct pool_state *find_pool(VkDevice device, VkCommandPool handle)
{
    for (struct pool_state *state = pools; state; state = state->next)
        if (state->handle == handle && state->device->handle == device) return state;
    return NULL;
}
static void missing(const char *name)
{
    fprintf(stderr, "libhybris vulkan: %s has no registered object-specific implementation\n", name);
    abort();
}
void hybris_render_dispatch_init(PFN_vkCreateDevice create, PFN_vkGetDeviceProcAddr resolve)
{
    create_device = create;
    device_proc = resolve;
}

VkResult vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                        const VkAllocationCallbacks *callbacks, VkDevice *device)
{
    if (!create_device || !device_proc) return VK_ERROR_INITIALIZATION_FAILED;
    size_t count = 0;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
        size_t n = info->pQueueCreateInfos[i].queueCount;
        if (n > SIZE_MAX - count) return VK_ERROR_OUT_OF_HOST_MEMORY;
        count += n;
    }
    if (count > (SIZE_MAX - sizeof(struct device_state)) / sizeof(struct queue_state))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct allocation allocation = allocation_for(callbacks);
    struct device_state *state = allocate(&allocation, sizeof(*state) + count * sizeof(struct queue_state),
                                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    state->allocation = allocation;
    state->queue_count = count;
    size_t slot = 0;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
        for (uint32_t j = 0; j < info->pQueueCreateInfos[i].queueCount; ++j)
            state->queues[slot++] = (struct queue_state){
                .family = info->pQueueCreateInfos[i].queueFamilyIndex,
                .index = j, .flags = info->pQueueCreateInfos[i].flags};
    VkResult result = create_device(physical, info, callbacks, device);
    if (result != VK_SUCCESS) { release(&allocation, state); return result; }
    state->handle = *device;
    for (unsigned i = 0; i < 8; ++i) state->render[i] = device_proc(*device, render_names[i]);
    pthread_mutex_lock(&guard);
    state->next = devices;
    devices = state;
    pthread_mutex_unlock(&guard);
    return result;
}

void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *callbacks)
{
    if (!device) return;
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)device_proc(device, "vkDestroyDevice");
    pthread_mutex_lock(&guard);
    struct device_state **link = &devices;
    while (*link && (*link)->handle != device) link = &(*link)->next;
    struct device_state *state = *link;
    if (state) *link = state->next;
    pthread_mutex_unlock(&guard);
    /* Valid callers have already destroyed all command pools. */
    if (!state || !destroy) missing("vkDestroyDevice");
    destroy(device, callbacks);
    struct allocation allocation = state->allocation;
    release(&allocation, state);
}
static void remember_queue(VkDevice device, uint32_t family, uint32_t index,
                           VkDeviceQueueCreateFlags flags, VkQueue queue)
{
    int found = 0;
    pthread_mutex_lock(&guard);
    struct device_state *state = find_device(device);
    if (state)
        for (size_t i = 0; i < state->queue_count; ++i)
            if (state->queues[i].family == family && state->queues[i].index == index &&
                state->queues[i].flags == flags) {
                state->queues[i].handle = queue;
                found = 1;
                break;
            }
    pthread_mutex_unlock(&guard);
    if (!found) missing("vkGetDeviceQueue");
}
void vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    PFN_vkGetDeviceQueue get = (PFN_vkGetDeviceQueue)device_proc(device, "vkGetDeviceQueue");
    if (!get) missing("vkGetDeviceQueue");
    get(device, family, index, queue);
    remember_queue(device, family, index, 0, *queue);
}
void vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    PFN_vkGetDeviceQueue2 get = (PFN_vkGetDeviceQueue2)device_proc(device, "vkGetDeviceQueue2");
    if (!get) missing("vkGetDeviceQueue2");
    get(device, info, queue);
    remember_queue(device, info->queueFamilyIndex, info->queueIndex, info->flags, *queue);
}
VkResult vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
                             const VkAllocationCallbacks *callbacks, VkCommandPool *pool)
{
    PFN_vkCreateCommandPool create = (PFN_vkCreateCommandPool)device_proc(device, "vkCreateCommandPool");
    pthread_mutex_lock(&guard);
    struct device_state *owner = find_device(device);
    pthread_mutex_unlock(&guard);
    if (!owner || !create) return VK_ERROR_INITIALIZATION_FAILED;
    struct allocation allocation = allocation_for(callbacks);
    struct pool_state *state = allocate(&allocation, sizeof(*state), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!state) return VK_ERROR_OUT_OF_HOST_MEMORY;
    state->allocation = allocation;
    state->device = owner;
    VkResult result = create(device, info, callbacks, pool);
    if (result != VK_SUCCESS) { release(&allocation, state); return result; }
    state->handle = *pool;
    pthread_mutex_lock(&guard);
    state->next = pools;
    pools = state;
    pthread_mutex_unlock(&guard);
    return result;
}
void vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *callbacks)
{
    if (!pool) return;
    PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)device_proc(device, "vkDestroyCommandPool");
    struct buffer_state *retired = NULL;
    pthread_mutex_lock(&guard);
    struct pool_state **link = &pools;
    while (*link && ((*link)->handle != pool || (*link)->device->handle != device)) link = &(*link)->next;
    struct pool_state *state = *link;
    if (state) {
        *link = state->next;
        struct buffer_state **buffer = &buffers;
        while (*buffer) {
            if ((*buffer)->pool == state) {
                struct buffer_state *entry = *buffer;
                *buffer = entry->next;
                entry->next = retired;
                retired = entry;
            } else buffer = &(*buffer)->next;
        }
    }
    pthread_mutex_unlock(&guard);
    if (!state || !destroy) missing("vkDestroyCommandPool");
    while (retired) {
        struct buffer_state *next = retired->next;
        release(&state->allocation, retired);
        retired = next;
    }
    destroy(device, pool, callbacks);
    struct allocation allocation = state->allocation;
    release(&allocation, state);
}
VkResult vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
                                  VkCommandBuffer *output)
{
    PFN_vkAllocateCommandBuffers call = (PFN_vkAllocateCommandBuffers)device_proc(device, "vkAllocateCommandBuffers");
    pthread_mutex_lock(&guard);
    struct pool_state *pool = find_pool(device, info->commandPool);
    pthread_mutex_unlock(&guard);
    if (!pool || !call) return VK_ERROR_INITIALIZATION_FAILED;
    struct buffer_state *pending = NULL;
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        struct buffer_state *entry = allocate(&pool->allocation, sizeof(*entry), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!entry) goto failed;
        entry->pool = pool;
        entry->next = pending;
        pending = entry;
    }
    result = call(device, info, output);
    if (result != VK_SUCCESS) goto failed;
    pthread_mutex_lock(&guard);
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        struct buffer_state *entry = pending;
        pending = entry->next;
        entry->handle = output[i];
        entry->next = buffers;
        buffers = entry;
    }
    pthread_mutex_unlock(&guard);
    return result;
failed:
    while (pending) {
        struct buffer_state *next = pending->next;
        release(&pool->allocation, pending);
        pending = next;
    }
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) output[i] = VK_NULL_HANDLE;
    return result;
}
void vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *handles)
{
    PFN_vkFreeCommandBuffers call = (PFN_vkFreeCommandBuffers)device_proc(device, "vkFreeCommandBuffers");
    struct buffer_state *retired = NULL;
    pthread_mutex_lock(&guard);
    struct pool_state *owner = find_pool(device, pool);
    for (uint32_t i = 0; i < count; ++i) {
        struct buffer_state **link = &buffers;
        while (*link && ((*link)->handle != handles[i] || (*link)->pool != owner)) link = &(*link)->next;
        if (*link) {
            struct buffer_state *entry = *link;
            *link = entry->next;
            entry->next = retired;
            retired = entry;
        }
    }
    pthread_mutex_unlock(&guard);
    if (!owner || !call) missing("vkFreeCommandBuffers");
    while (retired) {
        struct buffer_state *next = retired->next;
        release(&owner->allocation, retired);
        retired = next;
    }
    call(device, pool, count, handles);
}
static PFN_vkVoidFunction command_proc(VkCommandBuffer command, unsigned index)
{
    PFN_vkVoidFunction result = NULL;
    pthread_mutex_lock(&guard);
    for (struct buffer_state *state = buffers; state; state = state->next)
        if (state->handle == command) { result = state->pool->device->render[index]; break; }
    pthread_mutex_unlock(&guard);
    if (!result) missing(render_names[index]);
    return result;
}
static PFN_vkVoidFunction queue_proc(VkQueue queue, unsigned index)
{
    PFN_vkVoidFunction result = NULL;
    pthread_mutex_lock(&guard);
    for (struct device_state *state = devices; state && !result; state = state->next)
        for (size_t i = 0; i < state->queue_count; ++i)
            if (state->queues[i].handle == queue) { result = state->render[index]; break; }
    pthread_mutex_unlock(&guard);
    return result;
}
#define RENDER_WRAPPERS(suffix, offset) \
void vkCmdBeginRendering##suffix(VkCommandBuffer command, const VkRenderingInfo *info) { \
    ((PFN_vkCmdBeginRendering)command_proc(command, offset))(command, info); \
} \
void vkCmdEndRendering##suffix(VkCommandBuffer command) { \
    ((PFN_vkCmdEndRendering)command_proc(command, offset + 1))(command); \
} \
void vkCmdPipelineBarrier2##suffix(VkCommandBuffer command, const VkDependencyInfo *info) { \
    ((PFN_vkCmdPipelineBarrier2)command_proc(command, offset + 2))(command, info); \
} \
VkResult vkQueueSubmit2##suffix(VkQueue queue, uint32_t count, const VkSubmitInfo2 *info, VkFence fence) { \
    PFN_vkQueueSubmit2 call = (PFN_vkQueueSubmit2)queue_proc(queue, offset + 3); \
    return call ? call(queue, count, info, fence) : VK_ERROR_EXTENSION_NOT_PRESENT; \
}
RENDER_WRAPPERS(, 0)
RENDER_WRAPPERS(KHR, 4)
#undef RENDER_WRAPPERS

PFN_vkVoidFunction hybris_render_dispatch_proc(const char *name)
{
#define LOCAL(command) if (!strcmp(name, #command)) return (PFN_vkVoidFunction)command
    LOCAL(vkCreateDevice); LOCAL(vkDestroyDevice);
    LOCAL(vkGetDeviceQueue); LOCAL(vkGetDeviceQueue2);
    LOCAL(vkCreateCommandPool); LOCAL(vkDestroyCommandPool);
    LOCAL(vkAllocateCommandBuffers); LOCAL(vkFreeCommandBuffers);
    LOCAL(vkCmdBeginRendering); LOCAL(vkCmdEndRendering);
    LOCAL(vkCmdPipelineBarrier2); LOCAL(vkQueueSubmit2);
    LOCAL(vkCmdBeginRenderingKHR); LOCAL(vkCmdEndRenderingKHR);
    LOCAL(vkCmdPipelineBarrier2KHR); LOCAL(vkQueueSubmit2KHR);
#undef LOCAL
    return NULL;
}
