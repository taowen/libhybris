/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_VULKAN_LAYER_H
#define HYBRIS_VULKAN_LAYER_H
#include <vulkan/vulkan.h>

/* Vulkan state shared by every compatibility feature and driver. The layer
 * preserves driver handles and uses the loader dispatch key for child owners. */
struct hybris_layer_physical {
    VkInstance instance;
    uint64_t generation;
    uint32_t api_version;
    PFN_vkGetInstanceProcAddr resolver;
    unsigned application_policy;
};
/* Resolved once per device, before publication. Internal targets retain the
 * BC/shader/resource hooks; the command journal remains the outer wrapper. */
#define HYBRIS_LAYER_RECORDING_PROCS(X) \
    X(CmdBindPipeline, CmdBindPipeline) \
    X(CmdDraw, CmdDraw) \
    X(CmdDrawIndexed, CmdDrawIndexed) \
    X(CmdBindVertexBuffers, CmdBindVertexBuffers) \
    X(CmdBindVertexBuffers2, CmdBindVertexBuffers2) \
    X(CmdBindVertexBuffers2EXT, CmdBindVertexBuffers2EXT) \
    X(CmdBindIndexBuffer, CmdBindIndexBuffer) \
    X(CmdBindIndexBuffer2, CmdBindIndexBuffer2KHR) \
    X(CmdBindIndexBuffer2KHR, CmdBindIndexBuffer2KHR) \
    X(CmdSetViewport, CmdSetViewport) \
    X(CmdSetScissor, CmdSetScissor) \
    X(CmdSetViewportWithCount, CmdSetViewportWithCount) \
    X(CmdSetViewportWithCountEXT, CmdSetViewportWithCountEXT) \
    X(CmdSetScissorWithCount, CmdSetScissorWithCount) \
    X(CmdSetScissorWithCountEXT, CmdSetScissorWithCountEXT)
struct hybris_layer_command_dispatch {
#define HYBRIS_RECORDING_FIELD(n, type) PFN_vk##type n;
    HYBRIS_LAYER_RECORDING_PROCS(HYBRIS_RECORDING_FIELD)
#undef HYBRIS_RECORDING_FIELD
};
struct hybris_layer_device {
    struct hybris_dynamic_state *dynamic_rendering;
    int maintenance5;
    VkDevice handle;
    PFN_vkGetDeviceProcAddr resolver;
    VkPhysicalDevice physical;
    uint64_t generation;
    unsigned application_policy;
    int track_commands;
    int multi_viewport;
    int multi_draw;
    const struct hybris_layer_command_dispatch *recording;
    uint64_t instance_generation;
    /* Set only in command_device's returned copy, never in device state. */
    uint64_t recording_epoch;
};
int hybris_layer_lookup_physical(VkPhysicalDevice physical, struct hybris_layer_physical *out);
int hybris_layer_lookup_device(VkDevice device, struct hybris_layer_device *out);
int hybris_layer_lookup_queue(VkQueue queue, struct hybris_layer_device *out);
int hybris_layer_device_allocator(VkDevice device, VkAllocationCallbacks *allocator);
void *hybris_layer_dispatch_key(const void *handle);
void hybris_layer_trace_lifetime(const char *action, uint64_t generation,
    const void *handle, uint64_t instance_generation);
VkResult VKAPI_CALL hybris_layer_create_device(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *out);
void VKAPI_CALL hybris_layer_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator);
PFN_vkVoidFunction VKAPI_CALL hybris_layer_get_device_proc(VkDevice device, const char *name);
/* Resolve an inner function for any device-owned dispatchable handle. */
PFN_vkVoidFunction hybris_layer_handle_inner_proc(const void *handle, const char *name);
PFN_vkVoidFunction hybris_layer_device_inner_proc(VkDevice device, const char *name);
PFN_vkVoidFunction hybris_layer_device_dispatch(const char *name, int track_commands);
int hybris_layer_vertex_buffers2_proc(const char *name);
PFN_vkVoidFunction hybris_layer_physical_proc(const char *name);

int hybris_layer_command_device(VkCommandBuffer command, struct hybris_layer_device *device);
struct hybris_dynamic_recording;
struct hybris_dynamic_recording *hybris_layer_dynamic_recording(VkCommandBuffer command);
int hybris_layer_command_allocator(VkCommandBuffer command, VkAllocationCallbacks *allocator);
VkCommandPool hybris_layer_command_pool(VkCommandBuffer command);
void hybris_layer_command_error(VkCommandBuffer command, VkResult error);
/* Transfer an internal GPU resource to this recording on success only.
 * Cleanup runs outside registry locks when Vulkan permits reset/free, never
 * when recording ends or submission merely begins. Resource embeds its device
 * and allocator; the destroy callback must not access the retired journal. */
VkResult hybris_layer_command_retain(VkCommandBuffer command, void *resource,
    void (*destroy)(void *resource));
void hybris_layer_commands_release_device(VkDevice device);
struct hybris_polygon_recording;
struct hybris_polygon_recording *hybris_layer_polygon_recording(VkCommandBuffer command);
PFN_vkVoidFunction hybris_layer_commands_proc(const char *name);
PFN_vkVoidFunction hybris_layer_legacy_proc(VkDevice handle,const char *name);
#endif
