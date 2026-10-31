/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_POLYGON_BUFFERS_H
#define HYBRIS_POLYGON_BUFFERS_H
#include "polygon_pipeline.h"
#include "bc_context.h"
struct hybris_polygon_edges {
    VkDevice device;PFN_vkGetDeviceProcAddr resolver;VkAllocationCallbacks allocator;int custom;
    VkBuffer buffer;VkDeviceMemory memory;VkDescriptorPool descriptors;
    struct hybris_polygon_pipeline *pipeline;
    VkCommandBuffer pre;VkBuffer source;struct hybris_polygon_edges *next;
};
void hybris_polygon_edges_destroy(void *edges);
VkResult hybris_polygon_edges_create(struct hybris_bc_device *device,struct hybris_polygon_pipeline *pipeline,
    uint32_t count,uint32_t topology,const VkDescriptorBufferInfo *source,uint32_t byte_offset,
    uint32_t index_size,VkBool32 restart,VkCommandBuffer prepass,const VkAllocationCallbacks *allocator,
    struct hybris_polygon_edges **out);
#endif
