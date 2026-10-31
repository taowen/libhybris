/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_BC_CONTEXT_H
#define HYBRIS_BC_CONTEXT_H
#include "bc_transfer.h"
#include "polygon_indices.h"

#define HYBRIS_BC_IMAGE_BUCKETS 1024

struct hybris_bc_command {
    VkCommandBuffer handle;
    VkCommandPool pool;
    struct hybris_bc_command_state state;
    struct hybris_bc_transfer transfer;
    struct hybris_bc_command *next;
    struct hybris_bc_command *lookup_next;
    struct hybris_bc_device *owner;
};
struct hybris_bc_resource {
    VkImage handle;
    VkFormat format;
    VkImageType type;
    VkExtent3D extent;
    uint32_t levels, layers;
    VkSampleCountFlagBits samples;
    struct hybris_bc_image emulated;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct hybris_bc_resource *next, *previous, *lookup_next;
};
struct hybris_bc_device {
    VkDevice handle;
    VkPhysicalDevice physical;
    PFN_vkGetDeviceProcAddr resolver;
    /* Immutable native recording targets, resolved before device publication. */
    PFN_vkCmdBindPipeline bind_pipeline;
    PFN_vkCmdBindDescriptorSets bind_descriptors;
    PFN_vkCmdPushConstants push_constants;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceProperties properties;
    struct hybris_bc_decoder decoder;
    struct hybris_bc_layouts layouts;
    struct hybris_polygon_indices polygon;
    struct hybris_polygon_pattern *patterns[3], *retired_patterns;
    unsigned format_mask, rgb_mask;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    pthread_mutex_t guard;
    struct hybris_bc_resource *images;
    struct hybris_bc_resource *image_index[HYBRIS_BC_IMAGE_BUCKETS];
    struct hybris_bc_command *commands;
    struct hybris_bc_device *next;
};
VkResult hybris_bc_device_add(VkDevice handle, PFN_vkGetDeviceProcAddr resolver,
    const VkPhysicalDeviceMemoryProperties *memory, const VkPhysicalDeviceProperties *properties,
    unsigned format_mask, unsigned rgb_mask, const VkAllocationCallbacks *allocator);
void hybris_bc_device_remove(VkDevice handle);
struct hybris_bc_device *hybris_bc_device_find(VkDevice handle);
struct hybris_bc_command *hybris_bc_command_find(VkCommandBuffer handle, struct hybris_bc_device **owner);
/* Index mutations precede retirement; callers externally synchronize the
 * affected pool/command, just as for the underlying Vulkan object. */
void hybris_bc_command_publish(struct hybris_bc_device *device, struct hybris_bc_command *command);
void hybris_bc_command_unpublish(struct hybris_bc_command *command);
void hybris_bc_resource_publish(struct hybris_bc_device *device, struct hybris_bc_resource *resource);
struct hybris_bc_resource *hybris_bc_resource_remove(struct hybris_bc_device *device, VkImage image);
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
