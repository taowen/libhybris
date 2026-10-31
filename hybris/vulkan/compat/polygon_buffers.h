/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_POLYGON_BUFFERS_H
#define HYBRIS_POLYGON_BUFFERS_H
#include "polygon_pipeline.h"
#include "bc_context.h"
struct hybris_polygon_edges {
    VkAllocationCallbacks allocator;
    int custom;
    VkBuffer buffer;
    VkDeviceSize offset;
    struct hybris_polygon_pipeline *pipeline;
    VkBuffer source;
    struct hybris_polygon_edges *next;
};
struct hybris_polygon_storage;
void hybris_polygon_edges_destroy(void *edges);
void hybris_polygon_storage_reset(struct hybris_polygon_storage *storage);
void hybris_polygon_storage_destroy(struct hybris_polygon_storage *storage);
void hybris_polygon_patterns_destroy(struct hybris_bc_device *device);
VkResult hybris_polygon_edges_create(struct hybris_bc_device *device,struct hybris_polygon_pipeline *pipeline,
    struct hybris_polygon_storage **storage, uint32_t count,uint32_t topology,const VkDescriptorBufferInfo *source,uint32_t byte_offset,
    uint32_t index_size,VkBool32 restart,VkCommandBuffer prepass,const VkAllocationCallbacks *allocator,
    struct hybris_polygon_edges **out);
#endif
