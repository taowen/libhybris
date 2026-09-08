/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_CONTEXT_H
#define HYBRIS_BC_CONTEXT_H
#include "bc_transfer.h"

struct hybris_bc_command {
    VkCommandBuffer handle;
    VkCommandPool pool;
    struct hybris_bc_command_state state;
    struct hybris_bc_transfer transfer;
    struct hybris_bc_command *next;
};
struct hybris_bc_resource {
    VkImage handle;
    VkFormat format;
    VkImageType type;
    VkExtent3D extent;
    uint32_t levels, layers;
    struct hybris_bc_image emulated;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct hybris_bc_resource *next;
};
struct hybris_bc_device {
    VkDevice handle;
    PFN_vkGetDeviceProcAddr resolver;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceProperties properties;
    struct hybris_bc_decoder decoder;
    struct hybris_bc_layouts layouts;
    unsigned format_mask, rgb8_mask;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    pthread_mutex_t guard;
    struct hybris_bc_resource *images;
    struct hybris_bc_command *commands;
    struct hybris_bc_device *next;
};
VkResult hybris_bc_device_add(VkDevice handle, PFN_vkGetDeviceProcAddr resolver,
    const VkPhysicalDeviceMemoryProperties *memory, const VkPhysicalDeviceProperties *properties,
    unsigned format_mask, unsigned rgb8_mask, const VkAllocationCallbacks *allocator);
void hybris_bc_device_remove(VkDevice handle);
struct hybris_bc_device *hybris_bc_device_find(VkDevice handle);
struct hybris_bc_command *hybris_bc_command_find(VkCommandBuffer handle, struct hybris_bc_device **owner);
struct hybris_bc_resource *hybris_bc_resource_find(struct hybris_bc_device *device, VkImage handle);
void *hybris_bc_alloc(struct hybris_bc_device *device, size_t size);
void hybris_bc_free(struct hybris_bc_device *device, void *memory);
void hybris_bc_command_retire(struct hybris_bc_command *command);
PFN_vkVoidFunction hybris_bc_commands_proc(const char *name);
PFN_vkVoidFunction hybris_bc_resources_proc(const char *name);
PFN_vkVoidFunction hybris_bc_image_copy_proc(const char *name);
PFN_vkVoidFunction hybris_bc_record_proc(const char *name);
PFN_vkVoidFunction hybris_bc_barriers_proc(const char *name);
#endif
