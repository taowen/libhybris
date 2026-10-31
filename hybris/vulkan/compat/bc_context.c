/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct hybris_bc_device *devices;
/* Driver handles are preserved. Recording needs only one indexed bucket
 * lookup, rather than scanning every device and every command buffer. */
#define COMMAND_BUCKETS 1024
static struct hybris_bc_command *command_index[COMMAND_BUCKETS];
static size_t command_bucket(VkCommandBuffer handle)
{
    uintptr_t key = (uintptr_t)handle;
    key ^= key >> 17;
    key ^= key >> 9;
    return (key >> 4) & (COMMAND_BUCKETS - 1);
}
void hybris_bc_command_publish(struct hybris_bc_device *device, struct hybris_bc_command *command)
{
    size_t bucket = command_bucket(command->handle);
    pthread_mutex_lock(&guard);
    command->owner = device;
    command->lookup_next = command_index[bucket];
    command_index[bucket] = command;
    pthread_mutex_unlock(&guard);
}
void hybris_bc_command_unpublish(struct hybris_bc_command *command)
{
    pthread_mutex_lock(&guard);
    struct hybris_bc_command **link = &command_index[command_bucket(command->handle)];
    while (*link && *link != command) link = &(*link)->lookup_next;
    if (*link) *link = command->lookup_next;
    command->lookup_next = NULL;
    command->owner = NULL;
    pthread_mutex_unlock(&guard);
}
void *hybris_bc_alloc(struct hybris_bc_device *device, size_t size)
{
    void *memory = device->custom_allocator ? device->allocator.pfnAllocation(
        device->allocator.pUserData, size, _Alignof(max_align_t), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) : malloc(size);
    if (memory) memset(memory, 0, size);
    return memory;
}
void hybris_bc_free(struct hybris_bc_device *device, void *memory)
{
    if (!memory) return;
    if (device->custom_allocator) device->allocator.pfnFree(device->allocator.pUserData, memory);
    else free(memory);
}
static void init_recording_dispatch(struct hybris_bc_device *device)
{
    device->bind_pipeline = (PFN_vkCmdBindPipeline)device->resolver(device->handle, "vkCmdBindPipeline");
    device->bind_descriptors = (PFN_vkCmdBindDescriptorSets)device->resolver(device->handle, "vkCmdBindDescriptorSets");
    device->push_constants = (PFN_vkCmdPushConstants)device->resolver(device->handle, "vkCmdPushConstants");
}
VkResult hybris_bc_device_add(VkDevice handle, PFN_vkGetDeviceProcAddr resolver,
    const VkPhysicalDeviceMemoryProperties *memory, const VkPhysicalDeviceProperties *properties,
    unsigned format_mask, unsigned rgb_mask, const VkAllocationCallbacks *allocator)
{
    struct hybris_bc_device initial = {.handle = handle, .resolver = resolver,
        .memory = *memory, .properties = *properties, .format_mask = format_mask, .rgb_mask = rgb_mask,
        .custom_allocator = allocator != NULL};
    if (allocator) initial.allocator = *allocator;
    struct hybris_bc_device *device = hybris_bc_alloc(&initial, sizeof(*device));
    if (!device) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *device = initial;
    init_recording_dispatch(device);
    pthread_mutex_init(&device->guard, NULL);
    hybris_bc_layouts_init(&device->layouts, handle, resolver);
    VkResult result = VK_SUCCESS;
    if (format_mask) result = hybris_bc_decoder_create(handle, resolver, NULL, &device->decoder);
    if (result != VK_SUCCESS) {
        hybris_bc_layouts_finish(&device->layouts);
        pthread_mutex_destroy(&device->guard);
        hybris_bc_free(device, device);
        return result;
    }
    pthread_mutex_lock(&guard);
    device->next = devices;
    devices = device;
    pthread_mutex_unlock(&guard);
    return VK_SUCCESS;
}
struct hybris_bc_device *hybris_bc_device_find(VkDevice handle)
{
    pthread_mutex_lock(&guard);
    struct hybris_bc_device *device = devices;
    while (device && device->handle != handle) device = device->next;
    pthread_mutex_unlock(&guard);
    return device;
}
struct hybris_bc_command *hybris_bc_command_find(VkCommandBuffer handle, struct hybris_bc_device **owner)
{
    pthread_mutex_lock(&guard);
    struct hybris_bc_command *command = command_index[command_bucket(handle)];
    while (command && command->handle != handle) command = command->lookup_next;
    *owner = command ? command->owner : NULL;
    pthread_mutex_unlock(&guard);
    return command;
}
static size_t image_bucket(VkImage image)
{
    uint64_t key = (uint64_t)(uintptr_t)image;
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    return key & (HYBRIS_BC_IMAGE_BUCKETS - 1);
}
void hybris_bc_resource_publish(struct hybris_bc_device *device, struct hybris_bc_resource *resource)
{
    pthread_mutex_lock(&device->guard);
    size_t bucket = image_bucket(resource->handle);
    resource->lookup_next = device->image_index[bucket];
    device->image_index[bucket] = resource;
    resource->previous = NULL;
    resource->next = device->images;
    if (resource->next) resource->next->previous = resource;
    device->images = resource;
    pthread_mutex_unlock(&device->guard);
}
struct hybris_bc_resource *hybris_bc_resource_remove(struct hybris_bc_device *device, VkImage handle)
{
    pthread_mutex_lock(&device->guard);
    struct hybris_bc_resource **slot = &device->image_index[image_bucket(handle)];
    while (*slot && (*slot)->handle != handle) slot = &(*slot)->lookup_next;
    struct hybris_bc_resource *resource = *slot;
    if (resource) {
        *slot = resource->lookup_next;
        if (resource->previous) resource->previous->next = resource->next;
        else device->images = resource->next;
        if (resource->next) resource->next->previous = resource->previous;
        resource->next = resource->previous = resource->lookup_next = NULL;
    }
    pthread_mutex_unlock(&device->guard);
    return resource;
}
struct hybris_bc_resource *hybris_bc_resource_find(struct hybris_bc_device *device, VkImage handle)
{
    pthread_mutex_lock(&device->guard);
    struct hybris_bc_resource *image = device->image_index[image_bucket(handle)];
    while (image && image->handle != handle) image = image->lookup_next;
    pthread_mutex_unlock(&device->guard);
    return image;
}
void hybris_bc_command_retire(struct hybris_bc_command *command)
{
    hybris_bc_command_state_reset(&command->state);
    /* Devices without emulated formats have no decoder or internal resources. */
    if (command->transfer.decoder->device) hybris_bc_transfer_finish(&command->transfer);
}
void hybris_bc_device_remove(VkDevice handle)
{
    pthread_mutex_lock(&guard);
    struct hybris_bc_device **link = &devices;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    struct hybris_bc_device *device = *link;
    if (device) *link = device->next;
    pthread_mutex_unlock(&guard);
    if (!device) return;
    while (device->commands) {
        struct hybris_bc_command *next = device->commands->next;
        hybris_bc_command_unpublish(device->commands);
        hybris_bc_command_retire(device->commands);
        hybris_bc_free(device, device->commands);
        device->commands = next;
    }
    while (device->images) {
        struct hybris_bc_resource *next = device->images->next;
        if (device->images->emulated.image)
            hybris_bc_image_destroy(&device->images->emulated,
                device->images->custom_allocator ? &device->images->allocator : NULL);
        hybris_bc_free(device, device->images);
        device->images = next;
    }
    hybris_bc_layouts_finish(&device->layouts);
    if(device->polygon.device) hybris_polygon_indices_destroy(&device->polygon,NULL);
    if (device->decoder.device) hybris_bc_decoder_destroy(&device->decoder, NULL);
    pthread_mutex_destroy(&device->guard);
    hybris_bc_free(device, device);
}
