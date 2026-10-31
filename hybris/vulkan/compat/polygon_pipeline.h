/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_POLYGON_PIPELINE_H
#define HYBRIS_POLYGON_PIPELINE_H
#include "scaled_vertex.h"
struct hybris_polygon_pipeline {
    VkDevice device;PFN_vkGetDeviceProcAddr resolver;
    VkAllocationCallbacks allocator;unsigned custom_allocator;
    VkPipeline filled,variant;VkPolygonMode mode;VkPrimitiveTopology topology;
    VkBool32 restart,dynamic_topology,dynamic_restart;
    unsigned references;struct hybris_polygon_pipeline *next;
};
int hybris_polygon_emulated(VkPhysicalDevice physical);
void hybris_polygon_note(const char *reason);
VkResult hybris_polygon_pipeline_create(VkDevice device,PFN_vkGetDeviceProcAddr resolver,
    const VkGraphicsPipelineCreateInfo *graphics,VkPolygonMode mode,
    const VkAllocationCallbacks *allocator,struct hybris_polygon_pipeline **out);
VkResult hybris_polygon_pipeline_publish(struct hybris_polygon_pipeline *pipeline);
void hybris_polygon_pipeline_destroy(struct hybris_polygon_pipeline *pipeline);
struct hybris_polygon_pipeline *hybris_polygon_pipeline_acquire(VkDevice device,VkPipeline pipeline);
void hybris_polygon_pipeline_release(void *pipeline);
int hybris_polygon_pipeline_remove(VkDevice device,VkPipeline pipeline);
void hybris_polygon_pipelines_release_device(VkDevice device);
#endif
