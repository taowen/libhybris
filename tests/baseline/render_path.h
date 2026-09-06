#ifndef HYBRIS_PROBE_RENDER_PATH_H
#define HYBRIS_PROBE_RENDER_PATH_H
#include "probe.h"

/* family: 1 = Vulkan 1.3 core, 2 = Vulkan 1.1 + KHR extensions.
 * route: 0 = GIPA, 1 = GDPA, 2 = ELF dlsym, 3 = linked weak symbol. */
struct render_path {
  int family, route;
  VkPhysicalDeviceDynamicRenderingFeatures rendering;
  VkPhysicalDeviceSynchronization2Features synchronization;
  const char *extensions[4];
  PFN_vkCmdBeginRendering begin;
  PFN_vkCmdEndRendering end;
  PFN_vkCmdPipelineBarrier2 barrier;
  PFN_vkQueueSubmit2 submit;
};
uint32_t render_path_api(int family);
int render_path_enable(struct render_path *path, PFN_vkGetInstanceProcAddr gip,
                       VkInstance instance, VkPhysicalDevice physical, VkDeviceCreateInfo *create);
int render_path_resolve(struct render_path *path, void *library, PFN_vkGetInstanceProcAddr gip,
                        VkInstance instance, VkDevice device);
void render_path_begin(struct render_path *path, VkCommandBuffer cb, VkImage image,
                       VkImageView view, uint32_t width, uint32_t height);
void render_path_end(struct render_path *path, VkCommandBuffer cb, VkImage image);
void render_path_to_host(struct render_path *path, VkCommandBuffer cb);
VkResult render_path_submit(struct render_path *path, VkQueue queue,
                            VkCommandBuffer cb, VkFence fence);
#endif
