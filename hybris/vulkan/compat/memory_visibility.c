/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "memory_visibility.h"
#include "readback.h"
#include "application_policy.h"
#include "scaled_vertex.h"
#include "shader_dispatch.h"
#include "../layer/layer.h"
#include <pthread.h>
#include <string.h>

/* Blender's texture staging and immediate vertices omit noncoherent flushes.
 * Keep real memory properties; its render graph records copies/bindings after
 * CPU conversion. This is not general HOST_COHERENT emulation: these uploads
 * are host-written and immutable until their GPU consumers have retired. */
struct allocation {
    VkDevice device;
    uint64_t device_generation, generation, upload_revision;
    VkDeviceMemory memory;
    VkDeviceSize size, atom, map_offset, map_size;
    VkMemoryPropertyFlags properties;
    VkAllocationCallbacks allocator;
    int custom_allocator, mapped;
    struct allocation *next, *lookup_next;
};
struct buffer {
    VkDevice device;
    uint64_t device_generation, memory_generation;
    uint64_t generation, vertex_upload_epoch, vertex_upload_revision;
    VkBuffer handle;
    VkDeviceMemory memory;
    VkDeviceSize size, offset;
    VkBufferUsageFlags usage;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct buffer *next, *lookup_next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
/* Internal cache maintenance must also serialize with explicit application
 * flush/invalidate calls on mappings shared by several buffers. */
static pthread_mutex_t cache_guard = PTHREAD_MUTEX_INITIALIZER;
static struct allocation *allocations;
static struct buffer *buffers;
static uint64_t next_generation;
/* Keep ownership lists for device retirement, but never walk all live Vulkan
 * buffers/allocations for each draw, transfer or readback. Both indexes share
 * guard with the metadata and retain the existing device-generation checks. */
#define VISIBILITY_BUCKETS 4096
static struct allocation *memory_index[VISIBILITY_BUCKETS];
static struct buffer *buffer_index[VISIBILITY_BUCKETS];
static size_t visibility_bucket(VkDevice device, uint64_t handle)
{
    uint64_t key = handle ^ (uint64_t)(uintptr_t)device;
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    return (size_t)key & (VISIBILITY_BUCKETS - 1);
}
static void unindex_memory(struct allocation *allocation)
{
    struct allocation **link = &memory_index[visibility_bucket(allocation->device, (uint64_t)allocation->memory)];
    while (*link && *link != allocation) link = &(*link)->lookup_next;
    if (*link) *link = allocation->lookup_next;
}
static void unindex_buffer(struct buffer *buffer)
{
    struct buffer **link = &buffer_index[visibility_bucket(buffer->device, (uint64_t)buffer->handle)];
    while (*link && *link != buffer) link = &(*link)->lookup_next;
    if (*link) *link = buffer->lookup_next;
}
static struct allocation *find_memory(const struct hybris_layer_device *device, VkDeviceMemory memory)
{
    for (struct allocation *a = memory_index[visibility_bucket(device->handle, (uint64_t)memory)]; a; a = a->lookup_next)
        if (a->device == device->handle && a->device_generation == device->generation && a->memory == memory)
            return a;
    return NULL;
}
static struct buffer *find_buffer(const struct hybris_layer_device *device, VkBuffer handle)
{
    for (struct buffer *b = buffer_index[visibility_bucket(device->handle, (uint64_t)handle)]; b; b = b->lookup_next)
        if (b->device == device->handle && b->device_generation == device->generation && b->handle == handle)
            return b;
    return NULL;
}
VkResult hybris_memory_visibility_storage_buffer(const struct hybris_layer_device *device,
    VkBuffer handle, VkDeviceSize offset, VkDeviceSize size, VkDescriptorBufferInfo *out)
{
    *out = (VkDescriptorBufferInfo){0};
    VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
    pthread_mutex_lock(&guard);
    const struct buffer *b = find_buffer(device, handle);
    const struct allocation *a = b ? find_memory(device, b->memory) : NULL;
    if (!b || !(b->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !a ||
        !b->memory_generation || a->generation != b->memory_generation ||
        b->offset > a->size || b->size > a->size - b->offset || offset >= b->size) goto done;
    if (size == VK_WHOLE_SIZE) size = b->size - offset;
    if (!size || size > b->size - offset) goto done;
    *out = (VkDescriptorBufferInfo){handle, offset, size}; result = VK_SUCCESS;
done:
    pthread_mutex_unlock(&guard);
    return result;
}
static VkResult cache_memory(VkDevice device, uint32_t count,
    const VkMappedMemoryRange *ranges, const char *name)
{
    pthread_mutex_lock(&cache_guard);
    VkResult result = ((PFN_vkFlushMappedMemoryRanges)hybris_layer_device_inner_proc(device, name))(
        device, count, ranges);
    pthread_mutex_unlock(&cache_guard);
    return result;
}
static void changed_upload_mapping(struct allocation *allocation)
{
    /* Saturation disables reuse rather than aliasing an old revision. */
    if (allocation->upload_revision != UINT64_MAX) ++allocation->upload_revision;
}
static void explicit_cache_change(VkDevice handle, uint32_t count, const VkMappedMemoryRange *ranges)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
    if (!(device.application_policy & HYBRIS_APP_HOST_UPLOAD_FLUSH)) return;
    pthread_mutex_lock(&guard);
    for (uint32_t i = 0; i < count; ++i) {
        struct allocation *allocation = find_memory(&device, ranges[i].memory);
        if (allocation) changed_upload_mapping(allocation);
    }
    pthread_mutex_unlock(&guard);
}
static VkResult VKAPI_CALL flush_memory(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges)
{
    explicit_cache_change(device, count, ranges);
    return cache_memory(device, count, ranges, "vkFlushMappedMemoryRanges");
}
static VkResult VKAPI_CALL invalidate_memory(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges)
{
    explicit_cache_change(device, count, ranges);
    return cache_memory(device, count, ranges, "vkInvalidateMappedMemoryRanges");
}
VkResult hybris_memory_visibility_readback_range(const struct hybris_layer_device *device,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, struct hybris_readback_range *range)
{
    *range = (struct hybris_readback_range){0};
    if (!(device->application_policy & HYBRIS_APP_HOST_READBACK_INVALIDATE)) return VK_SUCCESS;
    VkResult result = VK_SUCCESS;
    pthread_mutex_lock(&guard);
    struct buffer *b = find_buffer(device, buffer);
    struct allocation *a = b ? find_memory(device, b->memory) : NULL;
    if (!b || b->usage != VK_BUFFER_USAGE_TRANSFER_DST_BIT || !a ||
        a->generation != b->memory_generation || !a->mapped ||
        (a->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) goto done;
    if (offset > b->size) { result = VK_ERROR_MEMORY_MAP_FAILED; goto done; }
    if (size == VK_WHOLE_SIZE) size = b->size - offset;
    if (!a->atom || size > b->size - offset || b->offset > a->size ||
        b->size > a->size - b->offset) { result = VK_ERROR_MEMORY_MAP_FAILED; goto done; }
    if (!size) goto done;
    VkDeviceSize begin = b->offset + offset, end = begin + size;
    begin -= begin % a->atom;
    VkDeviceSize padding = end % a->atom ? a->atom - end % a->atom : 0;
    end += padding < a->size - end ? padding : a->size - end;
    if (begin < a->map_offset || end > a->map_offset + a->map_size) {
        result = VK_ERROR_MEMORY_MAP_FAILED; goto done;
    }
    *range = (struct hybris_readback_range){a->memory, a->generation, begin, end - begin};
done:
    pthread_mutex_unlock(&guard);
    return result;
}
VkResult hybris_memory_visibility_invalidate(const struct hybris_layer_device *device,
    const struct hybris_readback_range *range)
{
    VkResult result = VK_SUCCESS;
    VkMappedMemoryRange mapped = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    pthread_mutex_lock(&guard);
    struct allocation *a = find_memory(device, range->memory);
    if (a && a->generation == range->generation && a->mapped) {
        if (range->offset < a->map_offset || range->offset > a->map_offset + a->map_size ||
            range->size > a->map_offset + a->map_size - range->offset)
            result = VK_ERROR_MEMORY_MAP_FAILED;
        else { mapped.memory = a->memory; mapped.offset = range->offset; mapped.size = range->size; }
    }
    pthread_mutex_unlock(&guard);
    if (mapped.memory) result = invalidate_memory(device->handle, 1, &mapped);
    return result;
}
static VkResult VKAPI_CALL allocate_memory(VkDevice handle, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *out)
{
    struct hybris_layer_device device;
    struct hybris_layer_physical physical;
    if (!hybris_layer_lookup_device(handle, &device) || !hybris_layer_lookup_physical(device.physical, &physical))
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
    VkResult result = ((PFN_vkAllocateMemory)hybris_layer_device_inner_proc(handle,
        "vkAllocateMemory"))(handle, &translated, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, a); return result; }
    a->memory = *out;
    pthread_mutex_lock(&guard);
    a->next = allocations;
    allocations = a;
    size_t bucket = visibility_bucket(handle, (uint64_t)a->memory);
    a->lookup_next = memory_index[bucket];
    memory_index[bucket] = a;
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL free_memory(VkDevice handle, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
    struct allocation *removed = NULL;
    pthread_mutex_lock(&guard);
    for (struct allocation **p = &allocations; *p; p = &(*p)->next)
        if ((*p)->device == handle && (*p)->memory == memory) {
            removed = *p; *p = removed->next; unindex_memory(removed); break;
        }
    pthread_mutex_unlock(&guard);
    ((PFN_vkFreeMemory)hybris_layer_device_inner_proc(handle, "vkFreeMemory"))(handle, memory, allocator);
    if (removed) hybris_scaled_free(removed->custom_allocator ? &removed->allocator : NULL, removed);
}
static VkResult VKAPI_CALL map_memory(VkDevice handle, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
    VkResult result = ((PFN_vkMapMemory)hybris_layer_device_inner_proc(handle,
        "vkMapMemory"))(handle, memory, offset, size, flags, out);
    struct hybris_layer_device device;
    if (result == VK_SUCCESS && hybris_layer_lookup_device(handle, &device)) {
        pthread_mutex_lock(&guard);
        struct allocation *a = find_memory(&device, memory);
        if (a) {
            changed_upload_mapping(a);
            a->mapped = 1; a->map_offset = offset; a->map_size = size == VK_WHOLE_SIZE ? a->size - offset : size;
        }
        pthread_mutex_unlock(&guard);
    }
    return result;
}
static void VKAPI_CALL unmap_memory(VkDevice handle, VkDeviceMemory memory)
{
    struct hybris_layer_device device;
    if (hybris_layer_lookup_device(handle, &device)) {
        pthread_mutex_lock(&guard);
        struct allocation *a = find_memory(&device, memory);
        if (a) { changed_upload_mapping(a); a->mapped = 0; }
        pthread_mutex_unlock(&guard);
    }
    ((PFN_vkUnmapMemory)hybris_layer_device_inner_proc(handle, "vkUnmapMemory"))(handle, memory);
}
static VkResult VKAPI_CALL create_buffer(VkDevice handle, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *out)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    VkBufferCreateInfo translated=*info;
    if((info->usage&VK_BUFFER_USAGE_INDEX_BUFFER_BIT) && !(info->flags&(VK_BUFFER_CREATE_SPARSE_BINDING_BIT|VK_BUFFER_CREATE_PROTECTED_BIT)) && hybris_shader_polygon_active(handle) && info->size<=UINT64_MAX-3) {
        translated.usage|=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;translated.size=(info->size+3)&~UINT64_C(3);
    }
    struct buffer *b = hybris_scaled_alloc(allocator, sizeof(*b), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *b = (struct buffer){.device = handle, .device_generation = device.generation,
        .size = translated.size, .usage = translated.usage, .custom_allocator = allocator != NULL};
    if (allocator) b->allocator = *allocator;
    pthread_mutex_lock(&guard);
    if (next_generation == UINT64_MAX) {
        pthread_mutex_unlock(&guard);
        hybris_scaled_free(allocator, b);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    b->generation = ++next_generation;
    pthread_mutex_unlock(&guard);
    VkResult result = ((PFN_vkCreateBuffer)hybris_layer_device_inner_proc(handle,
        "vkCreateBuffer"))(handle, &translated, allocator, out);
    if (result != VK_SUCCESS) { hybris_scaled_free(allocator, b); return result; }
    b->handle = *out;
    pthread_mutex_lock(&guard);
    b->next = buffers;
    buffers = b;
    size_t bucket = visibility_bucket(handle, (uint64_t)b->handle);
    b->lookup_next = buffer_index[bucket];
    buffer_index[bucket] = b;
    pthread_mutex_unlock(&guard);
    return result;
}
static void VKAPI_CALL destroy_buffer(VkDevice handle, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
    struct buffer *removed = NULL;
    pthread_mutex_lock(&guard);
    for (struct buffer **p = &buffers; *p; p = &(*p)->next)
        if ((*p)->device == handle && (*p)->handle == buffer) {
            removed = *p; *p = removed->next; unindex_buffer(removed); break;
        }
    pthread_mutex_unlock(&guard);
    ((PFN_vkDestroyBuffer)hybris_layer_device_inner_proc(handle, "vkDestroyBuffer"))(handle, buffer, allocator);
    if (removed) hybris_scaled_free(removed->custom_allocator ? &removed->allocator : NULL, removed);
}
static void bound(VkDevice handle, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
    pthread_mutex_lock(&guard);
    struct allocation *a = find_memory(&device, memory);
    struct buffer *b = find_buffer(&device, buffer);
    if (b) {
        b->vertex_upload_epoch = 0;
        b->memory = memory; b->offset = offset; b->memory_generation = a ? a->generation : 0;
    }
    pthread_mutex_unlock(&guard);
}
static VkResult VKAPI_CALL bind_buffer(VkDevice handle, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    VkResult result = ((PFN_vkBindBufferMemory)hybris_layer_device_inner_proc(handle,
        "vkBindBufferMemory"))(handle, buffer, memory, offset);
    if (result == VK_SUCCESS) bound(handle, buffer, memory, offset);
    return result;
}
static VkResult bind_buffers(VkDevice handle, uint32_t count, const VkBindBufferMemoryInfo *infos, const char *name)
{
    VkResult result = ((PFN_vkBindBufferMemory2)hybris_layer_device_inner_proc(handle, name))(handle, count, infos);
    if (result == VK_SUCCESS)
        for (uint32_t i = 0; i < count; ++i) bound(handle, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    return result;
}
static VkResult VKAPI_CALL bind_core(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *p)
{ return bind_buffers(d, n, p, "vkBindBufferMemory2"); }
static VkResult VKAPI_CALL bind_khr(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *p)
{ return bind_buffers(d, n, p, "vkBindBufferMemory2KHR"); }

static void flush_upload(VkCommandBuffer command, const struct hybris_layer_device *device, VkBuffer source, int upload_kind)
{
    if (!(device->application_policy & HYBRIS_APP_HOST_UPLOAD_FLUSH)) return;
    /* The existing Blender immediate-upload policy requires host writes to
     * precede recording and remain immutable until GPU retirement. Repeated
     * bindings in that recording do not make the same 4 MiB buffer dirty again.
     * Do not apply this reuse to texture or indirect uploads. */
    uint64_t epoch = upload_kind == 1 ? device->recording_epoch : 0;
    uint64_t buffer_generation = 0, memory_generation = 0, revision = 0;
    VkDeviceSize buffer_offset = 0;
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    VkResult result = VK_SUCCESS;
    pthread_mutex_lock(&guard);
    struct buffer *b = find_buffer(device, source);
    /* VMA maps entire pools, including GPU-written buffers. Match only the
     * observed texture (0), immediate vertex (1), or indirect parameter (2)
     * usage. A mapped pool alone does not identify host-written data. */
    VkBufferUsageFlags host_usage = upload_kind == 2 ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : upload_kind == 1 ?
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
            else if (!(epoch && a->upload_revision != UINT64_MAX &&
                       b->vertex_upload_epoch == epoch &&
                       b->vertex_upload_revision == a->upload_revision)) {
                range.memory = a->memory; range.offset = begin; range.size = end - begin;
                buffer_generation = b->generation; memory_generation = a->generation;
                revision = a->upload_revision; buffer_offset = b->offset;
            }
        }
    }
    pthread_mutex_unlock(&guard);
    /* Never invoke driver code or application allocation callbacks under guard. */
    if (range.memory) {
        result = cache_memory(device->handle, 1, &range, "vkFlushMappedMemoryRanges");
        if (result == VK_SUCCESS && epoch) {
            pthread_mutex_lock(&guard);
            b = find_buffer(device, source);
            a = find_memory(device, range.memory);
            if (b && a && b->generation == buffer_generation && b->memory == range.memory &&
                b->offset == buffer_offset && b->memory_generation == memory_generation &&
                a->generation == memory_generation && a->upload_revision == revision && a->mapped) {
                b->vertex_upload_epoch = epoch;
                b->vertex_upload_revision = revision;
            }
            pthread_mutex_unlock(&guard);
        }
    }
    if (result != VK_SUCCESS) hybris_layer_command_error(command, result);
}
static void VKAPI_CALL bind_vertices(VkCommandBuffer command, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    for (uint32_t i = 0; i < count; ++i)
        if (buffers[i]) flush_upload(command, &device, buffers[i], 1);
    ((PFN_vkCmdBindVertexBuffers)hybris_layer_device_inner_proc(device.handle,
        "vkCmdBindVertexBuffers"))(command, first, count, buffers, offsets);
}
static void bind_vertices2(VkCommandBuffer command, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets, const VkDeviceSize *sizes,
    const VkDeviceSize *strides, const char *name)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    for (uint32_t i = 0; i < count; ++i)
        if (buffers[i]) flush_upload(command, &device, buffers[i], 1);
    ((PFN_vkCmdBindVertexBuffers2)hybris_layer_device_inner_proc(device.handle,
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
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    flush_upload(command, &device, source, 0);
    ((PFN_vkCmdCopyBufferToImage)hybris_layer_device_inner_proc(device.handle,
        "vkCmdCopyBufferToImage"))(command, source, image, layout, count, regions);
}
static void copy2(VkCommandBuffer command, const VkCopyBufferToImageInfo2 *info, const char *name)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    flush_upload(command, &device, info->srcBuffer, 0);
    ((PFN_vkCmdCopyBufferToImage2)hybris_layer_device_inner_proc(device.handle, name))(command, info);
}
static void VKAPI_CALL copy_core(VkCommandBuffer c, const VkCopyBufferToImageInfo2 *i)
{ copy2(c, i, "vkCmdCopyBufferToImage2"); }
static void VKAPI_CALL copy_khr(VkCommandBuffer c, const VkCopyBufferToImageInfo2 *i)
{ copy2(c, i, "vkCmdCopyBufferToImage2KHR"); }
/* VKDrawList writes these parameters on the host. Compute-produced indirect
 * buffers have additional usage bits and must never be flushed here. */
static void VKAPI_CALL draw_indirect(VkCommandBuffer command, VkBuffer buffer,
    VkDeviceSize offset, uint32_t count, uint32_t stride)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    if (count) flush_upload(command, &device, buffer, 2);
    ((PFN_vkCmdDrawIndirect)hybris_layer_device_inner_proc(device.handle,
        "vkCmdDrawIndirect"))(command, buffer, offset, count, stride);
}
static void VKAPI_CALL draw_indexed_indirect(VkCommandBuffer command, VkBuffer buffer,
    VkDeviceSize offset, uint32_t count, uint32_t stride)
{
    struct hybris_layer_device device;
    if (!hybris_layer_command_device(command, &device)) return;
    if (count) flush_upload(command, &device, buffer, 2);
    ((PFN_vkCmdDrawIndexedIndirect)hybris_layer_device_inner_proc(device.handle,
        "vkCmdDrawIndexedIndirect"))(command, buffer, offset, count, stride);
}
void hybris_memory_visibility_release_device(VkDevice device)
{
    hybris_readback_release_submissions(device);
    struct allocation *alist = NULL;
    struct buffer *blist = NULL;
    pthread_mutex_lock(&guard);
    for (struct allocation **p = &allocations; *p;)
        if ((*p)->device == device) {
            struct allocation *a = *p; *p = a->next;
            unindex_memory(a); a->next = alist; alist = a;
        }
        else p = &(*p)->next;
    for (struct buffer **p = &buffers; *p;)
        if ((*p)->device == device) {
            struct buffer *b = *p; *p = b->next;
            unindex_buffer(b); b->next = blist; blist = b;
        }
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
    PROC(FlushMappedMemoryRanges, flush_memory); PROC(InvalidateMappedMemoryRanges, invalidate_memory);
    PROC(CreateBuffer, create_buffer); PROC(DestroyBuffer, destroy_buffer);
    PROC(BindBufferMemory, bind_buffer); PROC(BindBufferMemory2, bind_core); PROC(BindBufferMemory2KHR, bind_khr);
    PROC(CmdDrawIndirect, draw_indirect); PROC(CmdDrawIndexedIndirect, draw_indexed_indirect);
    PROC(CmdBindVertexBuffers, bind_vertices); PROC(CmdBindVertexBuffers2, vertices_core); PROC(CmdBindVertexBuffers2EXT, vertices_ext);
    PROC(CmdCopyBufferToImage, copy_buffer_image); PROC(CmdCopyBufferToImage2, copy_core); PROC(CmdCopyBufferToImage2KHR, copy_khr);
#undef PROC
    PFN_vkVoidFunction function = hybris_readback_record_proc(name);
    return function ? function : hybris_readback_submit_proc(name);
}
