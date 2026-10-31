/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HYBRIS_POLYGON_INDICES_H
#define HYBRIS_POLYGON_INDICES_H
#include <vulkan/vulkan.h>
struct hybris_polygon_indices {VkDevice device;PFN_vkGetDeviceProcAddr resolver;VkDescriptorSetLayout descriptors;VkPipelineLayout layout;VkPipeline pipeline;};
struct hybris_polygon_push {uint32_t byte_offset,index_size,triangles,topology,restart,row;};
VkResult hybris_polygon_indices_create(VkDevice device,PFN_vkGetDeviceProcAddr resolver,const VkAllocationCallbacks *allocator,struct hybris_polygon_indices *out);
void hybris_polygon_indices_destroy(struct hybris_polygon_indices *indices,const VkAllocationCallbacks *allocator);
#endif
