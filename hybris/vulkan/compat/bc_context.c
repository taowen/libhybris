/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct hybris_bc_device *devices;
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
    struct hybris_bc_command *command = NULL;
    *owner = NULL;
    pthread_mutex_lock(&guard);
    for (struct hybris_bc_device *device = devices; device && !command; device = device->next) {
        pthread_mutex_lock(&device->guard);
        for (command = device->commands; command && command->handle != handle; command = command->next) {}
        if (command) *owner = device;
        pthread_mutex_unlock(&device->guard);
    }
    pthread_mutex_unlock(&guard);
    return command;
}
struct hybris_bc_resource *hybris_bc_resource_find(struct hybris_bc_device *device, VkImage handle)
{
    pthread_mutex_lock(&device->guard);
    struct hybris_bc_resource *image = device->images;
    while (image && image->handle != handle) image = image->next;
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
    if (device->decoder.device) hybris_bc_decoder_destroy(&device->decoder, NULL);
    pthread_mutex_destroy(&device->guard);
    hybris_bc_free(device, device);
}
