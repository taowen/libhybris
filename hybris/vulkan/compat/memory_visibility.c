/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "memory_visibility.h"
#include "application_policy.h"
#include "scaled_vertex.h"
#include "../icd/commands.h"
#include "../icd/wsi.h"
#include <pthread.h>
#include <string.h>

/* Blender's texture staging and immediate vertices omit noncoherent flushes.
 * Keep real memory properties; its render graph records copies/bindings after
 * CPU conversion. This is not general HOST_COHERENT emulation: these uploads
 * are host-written and immutable until their GPU consumers have retired. */
struct allocation {
    VkDevice device;
    uint64_t device_generation, generation;
    VkDeviceMemory memory;
    VkDeviceSize size, atom, map_offset, map_size;
    VkMemoryPropertyFlags properties;
    VkAllocationCallbacks allocator;
    int custom_allocator, mapped;
    struct allocation *next;
};
struct buffer {
    VkDevice device;
    uint64_t device_generation, memory_generation;
    VkBuffer handle;
    VkDeviceMemory memory;
    VkDeviceSize size, offset;
    VkBufferUsageFlags usage;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct buffer *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct allocation *allocations;
static struct buffer *buffers;
static uint64_t next_generation;
static struct allocation *find_memory(const struct hybris_icd_device *device, VkDeviceMemory memory)
{
    for (struct allocation *a = allocations; a; a = a->next)
        if (a->device == device->handle && a->device_generation == device->generation && a->memory == memory)
            return a;
    return NULL;
}
static struct buffer *find_buffer(const struct hybris_icd_device *device, VkBuffer handle)
{
    for (struct buffer *b = buffers; b; b = b->next)
        if (b->device == device->handle && b->device_generation == device->generation && b->handle == handle)
            return b;
    return NULL;
}
static VkResult VKAPI_CALL allocate_memory(VkDevice handle, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *out)
{
    struct hybris_icd_device device;
    struct hybris_icd_physical physical;
    if (!hybris_icd_lookup_device(handle, &device) || !hybris_icd_lookup_physical(device.physical, &physical))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct allocation *a = hybris_scaled_alloc(allocator, sizeof(*a), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!a) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceProperties properties;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)physical.resolver(physical.instance,
        "vkGetPhysicalDeviceMemoryProperties"))(device.physical, &memory);
    ((PFN_vkGetPhysicalDeviceProperties)physical.resolver(physical.instance,
        "vkGetPhysicalDeviceProperties"))(device.physical, &properties);
    *a = (struct allocation){.device = handle, .device_generation = device.generation,
        .size = info->allocationSize, .atom = properties.limits.nonCoherentAtomSize,
        .properties = info->memoryTypeIndex < memory.memoryTypeCount ?
            memory.memoryTypes[info->memoryTypeIndex].propertyFlags : 0,
        .custom_allocator = allocator != NULL};
    if (allocator) a->allocator = *allocator;
    pthread_mutex_lock(&guard);
    if (next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&guard);
        hybris_scaled_free(allocator, a);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    a->generation = ++next_generation;
    pthread_mutex_unlock(&guard);
    VkResult result = ((PFN_vkAllocateMemory)hybris_icd_device_inner_proc(handle,
        "vkAllocateMemory"))(handle, info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, a); return result; }
    a->memory = *out;
    pthread_mutex_lock(&guard);
    a->next = allocations;
    allocations = a;
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL free_memory(VkDevice handle, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
    struct allocation *removed = NULL;
    pthread_mutex_lock(&guard);
    for (struct allocation **p = &allocations; *p; p = &(*p)->next)
        if ((*p)->device == handle && (*p)->memory == memory) { removed = *p; *p = removed->next; break; }
    pthread_mutex_unlock(&guard);
    ((PFN_vkFreeMemory)hybris_icd_device_inner_proc(handle, "vkFreeMemory"))(handle, memory, allocator);
    if (removed) hybris_scaled_free(removed->custom_allocator ? &removed->allocator : NULL, removed);
}
static VkResult VKAPI_CALL map_memory(VkDevice handle, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
    VkResult result = ((PFN_vkMapMemory)hybris_icd_device_inner_proc(handle,
        "vkMapMemory"))(handle, memory, offset, size, flags, out);
    struct hybris_icd_device device;
    if (result == VK_SUCCESS && hybris_icd_lookup_device(handle, &device)) {
        pthread_mutex_lock(&guard);
        struct allocation *a = find_memory(&device, memory);
        if (a) { a->mapped = 1; a->map_offset = offset; a->map_size = size == VK_WHOLE_SIZE ? a->size - offset : size; }
        pthread_mutex_unlock(&guard);
    }
    return result;
}
static void VKAPI_CALL unmap_memory(VkDevice handle, VkDeviceMemory memory)
{
    struct hybris_icd_device device;
    if (hybris_icd_lookup_device(handle, &device)) {
        pthread_mutex_lock(&guard);
        struct allocation *a = find_memory(&device, memory);
        if (a) a->mapped = 0;
        pthread_mutex_unlock(&guard);
    }
    ((PFN_vkUnmapMemory)hybris_icd_device_inner_proc(handle, "vkUnmapMemory"))(handle, memory);
}
static VkResult VKAPI_CALL create_buffer(VkDevice handle, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *out)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    struct buffer *b = hybris_scaled_alloc(allocator, sizeof(*b), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *b = (struct buffer){.device = handle, .device_generation = device.generation,
        .size = info->size, .usage = info->usage, .custom_allocator = allocator != NULL};
    if (allocator) b->allocator = *allocator;
    VkResult result = ((PFN_vkCreateBuffer)hybris_icd_device_inner_proc(handle,
        "vkCreateBuffer"))(handle, info, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, b); return result; }
    b->handle = *out;
    pthread_mutex_lock(&guard);
    b->next = buffers;
    buffers = b;
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL destroy_buffer(VkDevice handle, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
    struct buffer *removed = NULL;
    pthread_mutex_lock(&guard);
    for (struct buffer **p = &buffers; *p; p = &(*p)->next)
        if ((*p)->device == handle && (*p)->handle == buffer) { removed = *p; *p = removed->next; break; }
    pthread_mutex_unlock(&guard);
    ((PFN_vkDestroyBuffer)hybris_icd_device_inner_proc(handle, "vkDestroyBuffer"))(handle, buffer, allocator);
    if (removed) hybris_scaled_free(removed->custom_allocator ? &removed->allocator : NULL, removed);
}
static void bound(VkDevice handle, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    struct hybris_icd_device device;
    if (!hybris_icd_lookup_device(handle, &device)) return;
    pthread_mutex_lock(&guard);
    struct allocation *a = find_memory(&device, memory);
    struct buffer *b = find_buffer(&device, buffer);
    if (b) { b->memory = memory; b->offset = offset; b->memory_generation = a ? a->generation : 0; }
    pthread_mutex_unlock(&guard);
}
static VkResult VKAPI_CALL bind_buffer(VkDevice handle, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    VkResult result = ((PFN_vkBindBufferMemory)hybris_icd_device_inner_proc(handle,
        "vkBindBufferMemory"))(handle, buffer, memory, offset);
    if (result == VK_SUCCESS) bound(handle, buffer, memory, offset);
    return result;
}
static VkResult bind_buffers(VkDevice handle, uint32_t count, const VkBindBufferMemoryInfo *infos, const char *name)
{
    VkResult result = ((PFN_vkBindBufferMemory2)hybris_icd_device_inner_proc(handle, name))(handle, count, infos);
    if (result == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i) bound(handle, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    return result;
}
static VkResult VKAPI_CALL bind_core(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *p)
{ return bind_buffers(d, n, p, "vkBindBufferMemory2"); }
static VkResult VKAPI_CALL bind_khr(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *p)
{ return bind_buffers(d, n, p, "vkBindBufferMemory2KHR"); }

static void flush_upload(VkCommandBuffer command, const struct hybris_icd_device *device, VkBuffer source, int immediate)
{
    if (!(device->application_policy & HYBRIS_APP_HOST_UPLOAD_FLUSH)) return;
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    VkResult result = VK_SUCCESS;
    pthread_mutex_lock(&guard);
    struct buffer *b = find_buffer(device, source);
    /* VMA maps entire pools, including GPU-written buffers. Only the observed
     * immediate buffer usage identifies the host-written vertex path. Never
     * flush ordinary device vertex buffers merely because their pool is mapped. */
    VkBufferUsageFlags host_usage = immediate ?
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (b && b->usage != host_usage) {
        pthread_mutex_unlock(&guard);
        return;
    }
    struct allocation *a = b ? find_memory(device, b->memory) : NULL;
    if (!a || !b->memory_generation || a->generation != b->memory_generation)
        result = VK_ERROR_INITIALIZATION_FAILED;
    else if (a->mapped && !(a->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        if (!a->atom || b->offset > a->size || b->size > a->size - b->offset)
            result = VK_ERROR_MEMORY_MAP_FAILED;
        else {
            VkDeviceSize begin = b->offset - b->offset % a->atom;
            VkDeviceSize end = b->offset + b->size;
            VkDeviceSize padding = end % a->atom ? a->atom - end % a->atom : 0;
            end += padding < a->size - end ? padding : a->size - end;
            if (begin < a->map_offset || end > a->map_offset + a->map_size)
                result = VK_ERROR_MEMORY_MAP_FAILED;
            else { range.memory = a->memory; range.offset = begin; range.size = end - begin; }
        }
    }
    pthread_mutex_unlock(&guard);
    /* Never invoke driver code or application allocation callbacks under guard. */
    if (range.memory)
        result = ((PFN_vkFlushMappedMemoryRanges)hybris_icd_device_inner_proc(device->handle,
            "vkFlushMappedMemoryRanges"))(device->handle, 1, &range);
    if (result != VK_SUCCESS) hybris_icd_command_error(command, result);
}
static void VKAPI_CALL bind_vertices(VkCommandBuffer command, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    for (uint32_t i = 0; i < count; ++i)
        if (buffers[i]) flush_upload(command, &device, buffers[i], 1);
    ((PFN_vkCmdBindVertexBuffers)hybris_icd_device_inner_proc(device.handle,
        "vkCmdBindVertexBuffers"))(command, first, count, buffers, offsets);
}
static void bind_vertices2(VkCommandBuffer command, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets, const VkDeviceSize *sizes,
    const VkDeviceSize *strides, const char *name)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    for (uint32_t i = 0; i < count; ++i)
        if (buffers[i]) flush_upload(command, &device, buffers[i], 1);
    ((PFN_vkCmdBindVertexBuffers2)hybris_icd_device_inner_proc(device.handle,
        name))(command, first, count, buffers, offsets, sizes, strides);
}
static void VKAPI_CALL vertices_core(VkCommandBuffer c, uint32_t f, uint32_t n,
    const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *s, const VkDeviceSize *t)
{ bind_vertices2(c, f, n, b, o, s, t, "vkCmdBindVertexBuffers2"); }
static void VKAPI_CALL vertices_ext(VkCommandBuffer c, uint32_t f, uint32_t n,
    const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *s, const VkDeviceSize *t)
{ bind_vertices2(c, f, n, b, o, s, t, "vkCmdBindVertexBuffers2EXT"); }
static void VKAPI_CALL copy_buffer_image(VkCommandBuffer command, VkBuffer source,
    VkImage image, VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    flush_upload(command, &device, source, 0);
    ((PFN_vkCmdCopyBufferToImage)hybris_icd_device_inner_proc(device.handle,
        "vkCmdCopyBufferToImage"))(command, source, image, layout, count, regions);
}
static void copy2(VkCommandBuffer command, const VkCopyBufferToImageInfo2 *info, const char *name)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    flush_upload(command, &device, info->srcBuffer, 0);
    ((PFN_vkCmdCopyBufferToImage2)hybris_icd_device_inner_proc(device.handle, name))(command, info);
}
static void VKAPI_CALL copy_core(VkCommandBuffer c, const VkCopyBufferToImageInfo2 *i)
{ copy2(c, i, "vkCmdCopyBufferToImage2"); }
static void VKAPI_CALL copy_khr(VkCommandBuffer c, const VkCopyBufferToImageInfo2 *i)
{ copy2(c, i, "vkCmdCopyBufferToImage2KHR"); }
void hybris_memory_visibility_release_device(VkDevice device)
{
    struct allocation *alist = NULL;
    struct buffer *blist = NULL;
    pthread_mutex_lock(&guard);
    for (struct allocation **p = &allocations; *p;)
        if ((*p)->device == device) { struct allocation *a = *p; *p = a->next; a->next = alist; alist = a; }
        else p = &(*p)->next;
    for (struct buffer **p = &buffers; *p;)
        if ((*p)->device == device) { struct buffer *b = *p; *p = b->next; b->next = blist; blist = b; }
        else p = &(*p)->next;
    pthread_mutex_unlock(&guard);
    while (alist) { struct allocation *a = alist; alist = a->next; hybris_scaled_free(a->custom_allocator ? &a->allocator : NULL, a); }
    while (blist) { struct buffer *b = blist; blist = b->next; hybris_scaled_free(b->custom_allocator ? &b->allocator : NULL, b); }
}
PFN_vkVoidFunction hybris_memory_visibility_proc(const char *name)
{
#define PROC(n, f) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)f
    PROC(AllocateMemory, allocate_memory); PROC(FreeMemory, free_memory);
    PROC(MapMemory, map_memory); PROC(UnmapMemory, unmap_memory);
    PROC(CreateBuffer, create_buffer); PROC(DestroyBuffer, destroy_buffer);
    PROC(BindBufferMemory, bind_buffer); PROC(BindBufferMemory2, bind_core); PROC(BindBufferMemory2KHR, bind_khr);
    PROC(CmdBindVertexBuffers, bind_vertices); PROC(CmdBindVertexBuffers2, vertices_core); PROC(CmdBindVertexBuffers2EXT, vertices_ext);
    PROC(CmdCopyBufferToImage, copy_buffer_image); PROC(CmdCopyBufferToImage2, copy_core); PROC(CmdCopyBufferToImage2KHR, copy_khr);
#undef PROC
    return NULL;
}
