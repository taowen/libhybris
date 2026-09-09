/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "readback.h"
#include "memory_visibility.h"
#include "scaled_vertex.h"
#include "../icd/commands.h"
#include <pthread.h>
#include <string.h>

/* A recording owns these ranges until begin/reset/free. Submission takes an
 * independent snapshot, so resetting a retired command cannot erase pending
 * completion bookkeeping. Vulkan externally synchronizes each command buffer. */
struct recording {
    VkCommandBuffer command;
    VkCommandPool pool;
    VkDevice device;
    VkAllocationCallbacks allocator;
    int custom;
    struct hybris_readback_write *writes;
    struct recording *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct recording *recordings;

void hybris_readback_free_writes(struct hybris_readback_write *writes,
    const VkAllocationCallbacks *allocator)
{
    while (writes) {
        struct hybris_readback_write *next = writes->next;
        hybris_scaled_free(allocator, writes);
        writes = next;
    }
}
static VkResult append(struct hybris_readback_write **writes,
    const struct hybris_readback_range *range, const VkAllocationCallbacks *allocator)
{
    for (const struct hybris_readback_write *w = *writes; w; w = w->next)
        if (w->range.memory == range->memory && w->range.generation == range->generation &&
            w->range.offset == range->offset && w->range.size == range->size) return VK_SUCCESS;
    struct hybris_readback_write *w = hybris_scaled_alloc(allocator, sizeof(*w), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *w = (struct hybris_readback_write){.range = *range, .next = *writes};
    *writes = w;
    return VK_SUCCESS;
}
static struct recording *find(VkCommandBuffer command)
{
    pthread_mutex_lock(&guard);
    struct recording *r = recordings;
    while (r && r->command != command) r = r->next;
    pthread_mutex_unlock(&guard);
    return r;
}
static VkResult remember(VkCommandBuffer command, const struct hybris_readback_range *range)
{
    struct recording *r = find(command);
    if (!r) {
        struct hybris_icd_device device;
        if (!hybris_icd_command_device(command, &device)) return VK_ERROR_INITIALIZATION_FAILED;
        VkAllocationCallbacks allocator;
        int custom = hybris_icd_command_allocator(command, &allocator);
        r = hybris_scaled_alloc(custom ? &allocator : NULL, sizeof(*r), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
        if (!r) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *r = (struct recording){.command = command, .device = device.handle,
            .pool = hybris_icd_command_pool(command), .custom = custom};
        if (custom) r->allocator = allocator;
        pthread_mutex_lock(&guard);
        r->next = recordings;
        recordings = r;
        pthread_mutex_unlock(&guard);
    }
    return append(&r->writes, range, r->custom ? &r->allocator : NULL);
}
VkResult hybris_readback_collect(VkCommandBuffer command,
    struct hybris_readback_write **writes, const VkAllocationCallbacks *allocator)
{
    const struct recording *r = find(command);
    if (!r) return VK_SUCCESS;
    for (const struct hybris_readback_write *w = r->writes; w; w = w->next) {
        VkResult result = append(writes, &w->range, allocator);
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}
static void forget(VkCommandBuffer command, VkDevice device, VkCommandPool pool)
{
    struct recording *retired = NULL;
    pthread_mutex_lock(&guard);
    struct recording **link = &recordings;
    while (*link) {
        struct recording *r = *link;
        if (command ? r->command != command : r->device != device || r->pool != pool) {
            link = &r->next; continue;
        }
        *link = r->next;
        r->next = retired;
        retired = r;
    }
    pthread_mutex_unlock(&guard);
    while (retired) {
        struct recording *next = retired->next;
        const VkAllocationCallbacks *allocator = retired->custom ? &retired->allocator : NULL;
        hybris_readback_free_writes(retired->writes, allocator);
        hybris_scaled_free(allocator, retired);
        retired = next;
    }
}
void hybris_readback_reset_command(VkCommandBuffer command)
{ forget(command, VK_NULL_HANDLE, VK_NULL_HANDLE); }
void hybris_readback_reset_pool(VkDevice device, VkCommandPool pool)
{ forget(VK_NULL_HANDLE, device, pool); }

static void write_buffer(const struct hybris_icd_device *device, VkCommandBuffer command,
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
    struct hybris_readback_range range;
    VkResult result = hybris_memory_visibility_readback_range(device, buffer, offset, size, &range);
    if (result == VK_SUCCESS && range.memory) result = remember(command, &range);
    if (result != VK_SUCCESS) hybris_icd_command_error(command, result);
}
static void VKAPI_CALL copy_buffer(VkCommandBuffer command, VkBuffer source, VkBuffer dest,
    uint32_t count, const VkBufferCopy *regions)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    for (uint32_t i = 0; i < count; ++i)
        write_buffer(&device, command, dest, regions[i].dstOffset, regions[i].size);
    ((PFN_vkCmdCopyBuffer)hybris_icd_device_inner_proc(device.handle, "vkCmdCopyBuffer"))(
        command, source, dest, count, regions);
}
static void copy_buffer2(VkCommandBuffer command, const VkCopyBufferInfo2 *info, const char *name)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    for (uint32_t i = 0; i < info->regionCount; ++i)
        write_buffer(&device, command, info->dstBuffer, info->pRegions[i].dstOffset, info->pRegions[i].size);
    ((PFN_vkCmdCopyBuffer2)hybris_icd_device_inner_proc(device.handle, name))(command, info);
}
static void VKAPI_CALL copy_image(VkCommandBuffer command, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    /* Blender allocates a dedicated logical staging buffer for each image
     * read. Its complete buffer range excludes neighbouring VMA suballocations. */
    if (count) write_buffer(&device, command, buffer, 0, VK_WHOLE_SIZE);
    ((PFN_vkCmdCopyImageToBuffer)hybris_icd_device_inner_proc(device.handle, "vkCmdCopyImageToBuffer"))(
        command, image, layout, buffer, count, regions);
}
static void copy_image2(VkCommandBuffer command, const VkCopyImageToBufferInfo2 *info, const char *name)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    if (info->regionCount) write_buffer(&device, command, info->dstBuffer, 0, VK_WHOLE_SIZE);
    ((PFN_vkCmdCopyImageToBuffer2)hybris_icd_device_inner_proc(device.handle, name))(command, info);
}
static void VKAPI_CALL fill_buffer(VkCommandBuffer command, VkBuffer buffer,
    VkDeviceSize offset, VkDeviceSize size, uint32_t value)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    write_buffer(&device, command, buffer, offset, size);
    ((PFN_vkCmdFillBuffer)hybris_icd_device_inner_proc(device.handle, "vkCmdFillBuffer"))(
        command, buffer, offset, size, value);
}
static void VKAPI_CALL update_buffer(VkCommandBuffer command, VkBuffer buffer,
    VkDeviceSize offset, VkDeviceSize size, const void *data)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    write_buffer(&device, command, buffer, offset, size);
    ((PFN_vkCmdUpdateBuffer)hybris_icd_device_inner_proc(device.handle, "vkCmdUpdateBuffer"))(
        command, buffer, offset, size, data);
}
static void VKAPI_CALL execute_commands(VkCommandBuffer command, uint32_t count,
    const VkCommandBuffer *children)
{
    struct hybris_icd_device device;
    if (!hybris_icd_command_device(command, &device)) return;
    VkAllocationCallbacks allocator;
    int custom = hybris_icd_command_allocator(command, &allocator);
    struct hybris_readback_write *writes = NULL;
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i)
        result = hybris_readback_collect(children[i], &writes, custom ? &allocator : NULL);
    for (const struct hybris_readback_write *w = writes; w && result == VK_SUCCESS; w = w->next)
        result = remember(command, &w->range);
    hybris_readback_free_writes(writes, custom ? &allocator : NULL);
    if (result != VK_SUCCESS) hybris_icd_command_error(command, result);
    ((PFN_vkCmdExecuteCommands)hybris_icd_device_inner_proc(device.handle, "vkCmdExecuteCommands"))(
        command, count, children);
}
#define COPY2(suffix) \
static void VKAPI_CALL copy_buffer2_##suffix(VkCommandBuffer c, const VkCopyBufferInfo2 *i) \
{ copy_buffer2(c, i, "vkCmdCopyBuffer2" #suffix); } \
static void VKAPI_CALL copy_image2_##suffix(VkCommandBuffer c, const VkCopyImageToBufferInfo2 *i) \
{ copy_image2(c, i, "vkCmdCopyImageToBuffer2" #suffix); }
COPY2()
COPY2(KHR)
#undef COPY2
PFN_vkVoidFunction hybris_readback_record_proc(const char *name)
{
#define PROC(n, fn) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)fn
    PROC(CmdCopyBuffer, copy_buffer); PROC(CmdCopyBuffer2, copy_buffer2_); PROC(CmdCopyBuffer2KHR, copy_buffer2_KHR);
    PROC(CmdCopyImageToBuffer, copy_image); PROC(CmdCopyImageToBuffer2, copy_image2_);
    PROC(CmdCopyImageToBuffer2KHR, copy_image2_KHR);
    PROC(CmdFillBuffer, fill_buffer); PROC(CmdUpdateBuffer, update_buffer); PROC(CmdExecuteCommands, execute_commands);
#undef PROC
    return NULL;
}
