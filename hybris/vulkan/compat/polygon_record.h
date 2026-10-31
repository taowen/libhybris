/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_POLYGON_RECORD_H
#define HYBRIS_POLYGON_RECORD_H
#include "polygon_buffers.h"
struct hybris_polygon_link;
struct hybris_polygon_recording {
    VkCommandBuffer command;VkDevice device;PFN_vkGetDeviceProcAddr resolver;uint32_t family;
    VkAllocationCallbacks allocator;int custom;uint64_t epoch;
    VkCommandBufferUsageFlags usage;VkPipeline graphics;VkBuffer index;VkDeviceSize index_offset,index_size;
    VkIndexType index_type;VkPrimitiveTopology topology;VkBool32 restart;
    int topology_known,restart_known,unknown_writes,pre_open,has_prepass;VkCommandPool pre_pool;VkCommandBuffer pre_command;
    struct hybris_polygon_edges *edges;struct hybris_polygon_link *links;
};
VkResult hybris_polygon_record_end(VkCommandBuffer command);
void hybris_polygon_record_reset(struct hybris_polygon_recording *record);
void hybris_polygon_record_index(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset,VkDeviceSize size,VkIndexType type);
void hybris_polygon_record_written(VkCommandBuffer c,VkBuffer b);
void hybris_polygon_record_execute(VkCommandBuffer c,uint32_t count,const VkCommandBuffer *children);
uint32_t hybris_polygon_record_prepasses(VkCommandBuffer c,VkCommandBuffer *out,uint32_t capacity);
PFN_vkVoidFunction hybris_polygon_record_proc(const char *name);
VkResult hybris_polygon_submit(VkQueue queue,uint32_t count,const VkSubmitInfo *info,VkFence fence,PFN_vkQueueSubmit next);
VkResult hybris_polygon_submit2(VkQueue queue,uint32_t count,const VkSubmitInfo2 *info,VkFence fence,PFN_vkQueueSubmit2 next);
#endif
