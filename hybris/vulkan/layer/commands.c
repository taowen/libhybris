/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "../compat/multiviewport.h"
#include "layer.h"
#include "../compat/application_policy.h"
#include "../compat/scaled_vertex.h"
#include "../compat/readback.h"
#include "../compat/rendering_segments.h"
#include "../compat/shader_dispatch.h"
#include "../compat/memory_visibility.h"
#include "../compat/bc_context.h"
#include "../compat/polygon_record.h"
#include "../compat/dynamic_rendering.h"
#include <pthread.h>
#include <string.h>

/* Only compatibility metadata is owned here. Driver command handles and their
 * dispatch headers remain untouched. Pool/device destruction is externally
 * synchronized by Vulkan; the guard also protects unrelated devices/pools. */
struct retained_resource {
    void *resource;
    void (*destroy)(void *resource);
    struct retained_resource *next;
};
struct command_state {
    VkCommandBuffer handle;
    VkCommandPool pool;
    VkDevice device;
    uint64_t generation;
    uint64_t upload_epoch;
    VkResult error;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    VkCommandBufferLevel level;
    struct hybris_polygon_recording polygon;
    struct hybris_dynamic_recording dynamic_rendering;
    unsigned touched;
    struct retained_resource *resources;
    struct command_state *next;
    struct command_state *lookup_next;
};
struct pool_state {
    VkCommandPool handle;
    uint32_t family;
    VkDevice device;
    uint64_t generation;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct pool_state *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct command_state *commands;
static uint64_t next_upload_epoch;
static struct pool_state *pools;
#define COMMAND_BUCKETS 1024
static struct command_state *command_index[COMMAND_BUCKETS];
static size_t command_bucket(VkCommandBuffer handle)
{
    uintptr_t key = (uintptr_t)handle;
    key ^= key >> 17;
    key ^= key >> 9;
    return (key >> 4) & (COMMAND_BUCKETS - 1);
}
/* Caller holds guard. Keep hot lookups independent of the retirement list. */
static struct command_state *find_command_locked(VkCommandBuffer handle)
{
    struct command_state *state = command_index[command_bucket(handle)];
    while (state && state->handle != handle) state = state->lookup_next;
    return state;
}
/* Recording/freeing a particular command is externally synchronized by Vulkan. */
static struct command_state *find_command(VkCommandBuffer handle)
{
    pthread_mutex_lock(&guard);
    struct command_state *state=find_command_locked(handle);
    pthread_mutex_unlock(&guard);return state;
}

static PFN_vkVoidFunction inner(const struct hybris_layer_device *device, const char *name)
{
    return hybris_layer_device_inner_proc(device->handle, name);
}
static void destroy_resources(struct retained_resource *list, const VkAllocationCallbacks *allocator)
{
    while (list) {
        struct retained_resource *next = list->next;
        list->destroy(list->resource);
        hybris_scaled_free(allocator, list);
        list = next;
    }
}
static void reset_resources(VkCommandBuffer command)
{
    struct retained_resource *list = NULL;
    VkAllocationCallbacks allocator;
    int custom = 0;
    pthread_mutex_lock(&guard);
    for (struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) {
            state->upload_epoch = next_upload_epoch == UINT64_MAX ? 0 : ++next_upload_epoch;
            list = state->resources; state->resources = NULL;
            state->touched=0;
            custom = state->custom_allocator; if (custom) allocator = state->allocator;
            break;
        }
    pthread_mutex_unlock(&guard);
    destroy_resources(list, custom ? &allocator : NULL);
    struct command_state *state=find_command(command);if(state){hybris_polygon_record_reset(&state->polygon);state->polygon.epoch=state->upload_epoch;state->dynamic_rendering=(struct hybris_dynamic_recording){0};}
}
static void free_commands(struct command_state *list)
{
    while (list) {
        struct command_state *next = list->next;
        if (list->handle) hybris_readback_reset_command(list->handle);
        destroy_resources(list->resources, list->custom_allocator ? &list->allocator : NULL);
        hybris_polygon_record_reset(&list->polygon);
        hybris_scaled_free(list->custom_allocator ? &list->allocator : NULL, list);
        list = next;
    }
}

static VkResult VKAPI_CALL create_pool(VkDevice handle, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *out)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    struct pool_state *pool = hybris_scaled_alloc(allocator, sizeof(*pool), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pool = (struct pool_state){.device = handle, .generation = device.generation,.family=info->queueFamilyIndex,
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
int hybris_layer_command_device(VkCommandBuffer command, struct hybris_layer_device *device)
{
    VkDevice owner = VK_NULL_HANDLE;
    uint64_t generation = 0, epoch = 0;
    pthread_mutex_lock(&guard);
    const struct command_state *state = find_command_locked(command);
    if (state) { owner = state->device; generation = state->generation; epoch = state->upload_epoch; }
    pthread_mutex_unlock(&guard);
    if (!owner || !hybris_layer_lookup_device(owner, device) || device->generation != generation) return 0;
    device->recording_epoch = epoch;
    return 1;
}
int hybris_layer_command_allocator(VkCommandBuffer command, VkAllocationCallbacks *allocator)
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
VkCommandPool hybris_layer_command_pool(VkCommandBuffer command)
{
    VkCommandPool pool = VK_NULL_HANDLE;
    pthread_mutex_lock(&guard);
    for (const struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) { pool = state->pool; break; }
    pthread_mutex_unlock(&guard);
    return pool;
}
void hybris_layer_command_error(VkCommandBuffer command, VkResult error)
{
    pthread_mutex_lock(&guard);
    for (struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) {
            if (error == VK_SUCCESS || state->error == VK_SUCCESS) state->error = error;
            break;
        }
    pthread_mutex_unlock(&guard);
}
VkResult hybris_layer_command_retain(VkCommandBuffer command, void *resource,
    void (*destroy)(void *resource))
{
    if (!resource || !destroy) return VK_ERROR_INITIALIZATION_FAILED;
    VkAllocationCallbacks allocator;
    const VkAllocationCallbacks *a = hybris_layer_command_allocator(command, &allocator) ? &allocator : NULL;
    struct retained_resource *owned = hybris_scaled_alloc(a, sizeof(*owned), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!owned) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *owned = (struct retained_resource){.resource = resource, .destroy = destroy};
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&guard);
    for (struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) {
            owned->next = state->resources; state->resources = owned;
            result = VK_SUCCESS; break;
        }
    pthread_mutex_unlock(&guard);
    if (result != VK_SUCCESS) hybris_scaled_free(a, owned);
    return result;
}

static VkResult VKAPI_CALL allocate_commands(VkDevice handle, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *out)
{
    /* Allocation can fail in adapter metadata before the driver is called.
     * The command-buffer allocation contract still requires every output to
     * be null on failure, including entries not reached by partial staging. */
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) out[i] = VK_NULL_HANDLE;
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
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
            .custom_allocator = pool.custom_allocator, .level=info->level, .next = list};
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
        state->polygon=(struct hybris_polygon_recording){.device=handle,.resolver=device.resolver,
            .family=pool.family,.custom=pool.custom_allocator,.allocator=pool.allocator};
        size_t bucket = command_bucket(state->handle);
        state->lookup_next = command_index[bucket];
        command_index[bucket] = state;
        state->next = commands;
        commands = state;
    }
    pthread_mutex_unlock(&guard);
    return result;
}

static struct command_state *detach(const struct hybris_layer_device *device, VkCommandPool pool,
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
        struct command_state **indexed = &command_index[command_bucket(state->handle)];
        while (*indexed && *indexed != state) indexed = &(*indexed)->lookup_next;
        if (*indexed) *indexed = state->lookup_next;
        state->lookup_next = NULL;
        state->next = retired;
        retired = state;
    }
    pthread_mutex_unlock(&guard);
    return retired;
}
static void VKAPI_CALL release_commands(VkDevice handle, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer *list)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
    PFN_vkFreeCommandBuffers function = (PFN_vkFreeCommandBuffers)inner(&device, "vkFreeCommandBuffers");
    function(handle, pool, count, list);
    for (uint32_t i = 0; i < count; ++i)
        if (list[i]) free_commands(detach(&device, pool, list[i], 0));
}
static void VKAPI_CALL destroy_pool(VkDevice handle, VkCommandPool pool, const VkAllocationCallbacks *callbacks)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
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
void hybris_layer_commands_release_device(VkDevice handle)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
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
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
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
    if (result == VK_SUCCESS) {
        hybris_layer_command_error(command, VK_SUCCESS);
        reset_resources(command);
        struct command_state *state=find_command(command);if(state)state->polygon.usage=info->flags;
        hybris_readback_reset_command(command);
    }
    return result;
}
static VkResult VKAPI_CALL end_command(VkCommandBuffer command)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEndCommandBuffer function = (PFN_vkEndCommandBuffer)inner(&device, "vkEndCommandBuffer");
    VkResult pre=hybris_polygon_record_end(command);
    if(pre!=VK_SUCCESS)hybris_layer_command_error(command,pre);
    VkResult result = function(command);
    if (result != VK_SUCCESS) return result;
    pthread_mutex_lock(&guard);
    for (const struct command_state *state = commands; state; state = state->next)
        if (state->handle == command) { result = state->error; break; }
    pthread_mutex_unlock(&guard);
    return result;
}
static VkResult VKAPI_CALL reset_command(VkCommandBuffer command, VkCommandBufferResetFlags flags)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = ((PFN_vkResetCommandBuffer)inner(&device, "vkResetCommandBuffer"))(command, flags);
    if (result == VK_SUCCESS) {
        hybris_layer_command_error(command, VK_SUCCESS);
        reset_resources(command);
        hybris_readback_reset_command(command);
    }
    return result;
}
static VkResult VKAPI_CALL reset_pool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    VkResult result = ((PFN_vkResetCommandPool)hybris_layer_device_inner_proc(device,
        "vkResetCommandPool"))(device, pool, flags);
    if (result == VK_SUCCESS) {
        /* Detach one recording at a time; GPU/application callbacks must never
         * execute under the registry mutex shared with unrelated pools. */
        for (;;) {
            VkCommandBuffer command = VK_NULL_HANDLE;
            pthread_mutex_lock(&guard);
            for (struct command_state *state = commands; state; state = state->next)
                if (state->device == device && state->pool == pool && (state->resources || state->touched)) {
                    command = state->handle; break;
                }
            pthread_mutex_unlock(&guard);
            if (!command) break;
            reset_resources(command);
        }
        pthread_mutex_lock(&guard);
        for (struct command_state *state = commands; state; state = state->next)
            if (state->device == device && state->pool == pool) state->error = VK_SUCCESS;
        pthread_mutex_unlock(&guard);
        hybris_readback_reset_pool(device, pool);
    }
    return result;
}

static void begin_rendering(VkCommandBuffer command, const VkRenderingInfo *info, const char *name)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    PFN_vkCmdBeginRendering begin = (PFN_vkCmdBeginRendering)hybris_dynamic_proc(&device,name);
    if(!begin)begin=(PFN_vkCmdBeginRendering)device.resolver(device.handle,name);
    if(!begin){hybris_layer_command_error(command,VK_ERROR_FEATURE_NOT_PRESENT);return;}
    if (!(device.application_policy & HYBRIS_APP_RENDERING_SEGMENTS)) {
        begin(command, info);
        return;
    }
    VkAllocationCallbacks command_allocator;
    const VkAllocationCallbacks *allocator = hybris_layer_command_allocator(command, &command_allocator) ?
        &command_allocator : NULL;
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)
        device.resolver(device.handle, "vkCmdPipelineBarrier");
    VkResult result = hybris_rendering_segments_begin(command, info, begin, barrier, allocator);
    if (result != VK_SUCCESS) {
        hybris_layer_command_error(command, result);
        begin(command, info);
    }
}
static void VKAPI_CALL begin_core(VkCommandBuffer command, const VkRenderingInfo *info)
{ begin_rendering(command, info, "vkCmdBeginRendering"); }
static void VKAPI_CALL begin_khr(VkCommandBuffer command, const VkRenderingInfo *info)
{ begin_rendering(command, info, "vkCmdBeginRenderingKHR"); }
static void end_rendering(VkCommandBuffer command,const char *name)
{
    struct hybris_layer_device device;
    if(!hybris_layer_command_device(command,&device)) return;
    PFN_vkCmdEndRendering end=(void*)hybris_dynamic_proc(&device,name);
    if(!end)end=(void*)device.resolver(device.handle,name);
    if(!end){hybris_layer_command_error(command,VK_ERROR_FEATURE_NOT_PRESENT);return;}
    end(command);
}
static void VKAPI_CALL end_core(VkCommandBuffer c) {end_rendering(c,"vkCmdEndRendering");}
static void VKAPI_CALL end_khr(VkCommandBuffer c) {end_rendering(c,"vkCmdEndRenderingKHR");}
static void VKAPI_CALL bind_vertices(VkCommandBuffer c,uint32_t first,uint32_t count,
    const VkBuffer *buffers,const VkDeviceSize *offsets)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device)) return;
    device.recording->CmdBindVertexBuffers(c,first,count,buffers,offsets);
}
static void bind_vertices2(VkCommandBuffer c,uint32_t first,uint32_t count,const VkBuffer *buffers,
    const VkDeviceSize *offsets,const VkDeviceSize *sizes,const VkDeviceSize *strides,int extension)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device)) return;
    (extension ? device.recording->CmdBindVertexBuffers2EXT : device.recording->CmdBindVertexBuffers2)(c,first,count,buffers,offsets,sizes,strides);
}
static void VKAPI_CALL vertices_core(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,
    const VkDeviceSize *o,const VkDeviceSize *s,const VkDeviceSize *t)
{bind_vertices2(c,f,n,b,o,s,t,0);}
static void VKAPI_CALL vertices_ext(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,
    const VkDeviceSize *o,const VkDeviceSize *s,const VkDeviceSize *t)
{bind_vertices2(c,f,n,b,o,s,t,1);}
static void index_state(VkCommandBuffer c,VkBuffer buffer,VkDeviceSize offset,VkDeviceSize size,VkIndexType type)
{
    hybris_polygon_record_index(c,buffer,offset,size,type);

}
static void VKAPI_CALL bind_index(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device)) return;
    device.recording->CmdBindIndexBuffer(c,b,o,t);
    index_state(c,b,o,VK_WHOLE_SIZE,t);
}
static void bind_index2(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize s,VkIndexType t,int extension)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device)) return;
    (extension ? device.recording->CmdBindIndexBuffer2KHR : device.recording->CmdBindIndexBuffer2)(c,b,o,s,t);
    index_state(c,b,o,s,t);
}
static void VKAPI_CALL index_core(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize s,VkIndexType t)
{bind_index2(c,b,o,s,t,0);}
static void VKAPI_CALL index_khr(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize s,VkIndexType t)
{bind_index2(c,b,o,s,t,1);}
static void VKAPI_CALL viewport(VkCommandBuffer c,uint32_t first,uint32_t count,const VkViewport *viewports)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device)) return;
    if(device.multi_viewport) { count=hybris_multiviewport_count(2,first,count);if(first)return; }
    device.recording->CmdSetViewport(c,first,count,viewports);

}
static void VKAPI_CALL scissor(VkCommandBuffer c,uint32_t first,uint32_t count,const VkRect2D *rects)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device))return;
    if(device.multi_viewport) {count=hybris_multiviewport_count(3,first,count);if(first)return;}
    device.recording->CmdSetScissor(c,first,count,rects);
}
static void viewport_count(VkCommandBuffer c,uint32_t count,const VkViewport *views,int ext)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device))return;
    if(device.multi_viewport)count=hybris_multiviewport_count(4,0,count);
    PFN_vkCmdSetViewportWithCount function=ext?device.recording->CmdSetViewportWithCountEXT:device.recording->CmdSetViewportWithCount;
    function(c,count,views);

}
static void scissor_count(VkCommandBuffer c,uint32_t count,const VkRect2D *rects,int ext)
{
    struct hybris_layer_device device;if(!hybris_layer_command_device(c,&device))return;
    if(device.multi_viewport)count=hybris_multiviewport_count(5,0,count);
    PFN_vkCmdSetScissorWithCount function=ext?device.recording->CmdSetScissorWithCountEXT:device.recording->CmdSetScissorWithCount;
    function(c,count,rects);
}
static void VKAPI_CALL viewport_count_core(VkCommandBuffer c,uint32_t n,const VkViewport *v){viewport_count(c,n,v,0);}
static void VKAPI_CALL viewport_count_ext(VkCommandBuffer c,uint32_t n,const VkViewport *v){viewport_count(c,n,v,1);}
static void VKAPI_CALL scissor_count_core(VkCommandBuffer c,uint32_t n,const VkRect2D *v){scissor_count(c,n,v,0);}
static void VKAPI_CALL scissor_count_ext(VkCommandBuffer c,uint32_t n,const VkRect2D *v){scissor_count(c,n,v,1);}
PFN_vkVoidFunction hybris_layer_commands_proc(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction function; } entries[] = {
        {"vkCmdBeginRendering", (PFN_vkVoidFunction)begin_core},
        {"vkCmdBeginRenderingKHR", (PFN_vkVoidFunction)begin_khr},
        {"vkCmdEndRendering", (PFN_vkVoidFunction)end_core},
        {"vkCmdEndRenderingKHR", (PFN_vkVoidFunction)end_khr},
        {"vkCmdBindVertexBuffers", (PFN_vkVoidFunction)bind_vertices},
        {"vkCmdBindVertexBuffers2", (PFN_vkVoidFunction)vertices_core},
        {"vkCmdBindVertexBuffers2EXT", (PFN_vkVoidFunction)vertices_ext},
        {"vkCmdBindIndexBuffer", (PFN_vkVoidFunction)bind_index},
        {"vkCmdBindIndexBuffer2", (PFN_vkVoidFunction)index_core},
        {"vkCmdBindIndexBuffer2KHR", (PFN_vkVoidFunction)index_khr},
        {"vkCmdSetViewport", (PFN_vkVoidFunction)viewport},
        {"vkCmdSetScissor", (PFN_vkVoidFunction)scissor},
        {"vkCmdSetViewportWithCount", (PFN_vkVoidFunction)viewport_count_core},
        {"vkCmdSetViewportWithCountEXT", (PFN_vkVoidFunction)viewport_count_ext},
        {"vkCmdSetScissorWithCount", (PFN_vkVoidFunction)scissor_count_core},
        {"vkCmdSetScissorWithCountEXT", (PFN_vkVoidFunction)scissor_count_ext},
        {"vkCreateCommandPool", (PFN_vkVoidFunction)create_pool},
        {"vkAllocateCommandBuffers", (PFN_vkVoidFunction)allocate_commands},
        {"vkFreeCommandBuffers", (PFN_vkVoidFunction)release_commands},
        {"vkDestroyCommandPool", (PFN_vkVoidFunction)destroy_pool},
        {"vkBeginCommandBuffer", (PFN_vkVoidFunction)begin_command},
        {"vkEndCommandBuffer", (PFN_vkVoidFunction)end_command},
        {"vkResetCommandBuffer", (PFN_vkVoidFunction)reset_command},
        {"vkResetCommandPool", (PFN_vkVoidFunction)reset_pool},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(*entries); ++i)
        if (!strcmp(name, entries[i].name)) return entries[i].function;
    return hybris_polygon_record_proc(name);
}

struct hybris_polygon_recording *hybris_layer_polygon_recording(VkCommandBuffer command)
{struct command_state *state=find_command(command);if(state)state->touched=1;return state?&state->polygon:NULL;}
struct hybris_dynamic_recording *hybris_layer_dynamic_recording(VkCommandBuffer command)
{struct command_state *state=find_command(command);if(state)state->touched=1;return state?&state->dynamic_rendering:NULL;}
