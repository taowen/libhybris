#include "render_path.h"

#ifdef HYBRIS_PROBE_LINKED
/* Optional exports must not stop unrelated probes on an older loader. */
#pragma weak vkCmdBeginRendering
#pragma weak vkCmdBeginRenderingKHR
#pragma weak vkCmdEndRendering
#pragma weak vkCmdEndRenderingKHR
#pragma weak vkCmdPipelineBarrier2
#pragma weak vkCmdPipelineBarrier2KHR
#pragma weak vkQueueSubmit2
#pragma weak vkQueueSubmit2KHR
#endif

uint32_t render_path_api(int family) {
  return family == 1 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
}

int render_path_enable(struct render_path *path, PFN_vkGetInstanceProcAddr gip,
                       VkInstance instance, VkPhysicalDevice physical, VkDeviceCreateInfo *create) {
  V(vkGetPhysicalDeviceProperties);
  VkPhysicalDeviceProperties properties;
  p_vkGetPhysicalDeviceProperties(physical, &properties);
  if (properties.apiVersion < render_path_api(path->family)) {
    printf("UNSUPPORTED rendering physical API version\n");
    return 3;
  }
  if (path->family == 2) {
    /* Deliberately exercise the extension route at API 1.1, including all
     * dynamic-rendering dependencies that were promoted only in API 1.2. */
    path->extensions[0] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    path->extensions[1] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    path->extensions[2] = VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME;
    path->extensions[3] = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
    V(vkEnumerateDeviceExtensionProperties);
    uint32_t count = 0;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (count && !extensions) return 2;
    VkResult result = p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions);
    if (result != VK_SUCCESS) { free(extensions); return 2; }
    for (unsigned wanted = 0; wanted < 4; ++wanted) {
      int found = 0;
      for (unsigned i = 0; i < count; ++i)
        found |= !strcmp(extensions[i].extensionName, path->extensions[wanted]);
      if (!found) {
        printf("UNSUPPORTED rendering extension %s\n", path->extensions[wanted]);
        free(extensions);
        return 3;
      }
    }
    free(extensions);
    create->enabledExtensionCount = 4;
    create->ppEnabledExtensionNames = path->extensions;
  }
  path->synchronization.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
  path->rendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
  path->rendering.pNext = &path->synchronization;
  VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &path->rendering};
  V(vkGetPhysicalDeviceFeatures2);
  p_vkGetPhysicalDeviceFeatures2(physical, &features);
  printf("RENDER_FEATURES dynamicRendering=%u synchronization2=%u family=%s\n",
         path->rendering.dynamicRendering, path->synchronization.synchronization2,
         path->family == 1 ? "core13" : "khr11");
  if (!path->rendering.dynamicRendering || !path->synchronization.synchronization2) return 3;
  create->pNext = &path->rendering;
  return 0;
}

int render_path_resolve(struct render_path *path, void *library, PFN_vkGetInstanceProcAddr gip,
                        VkInstance instance, VkDevice device) {
  V(vkGetDeviceProcAddr);
  PFN_vkVoidFunction linked[4] = {0};
#ifdef HYBRIS_PROBE_LINKED
#define LINKED(name) (PFN_vkVoidFunction)(path->family == 1 ? name : name##KHR)
  linked[0] = LINKED(vkCmdBeginRendering);
  linked[1] = LINKED(vkCmdEndRendering);
  linked[2] = LINKED(vkCmdPipelineBarrier2);
  linked[3] = LINKED(vkQueueSubmit2);
#undef LINKED
#endif
  const char *core[] = {"vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdPipelineBarrier2", "vkQueueSubmit2"};
  const char *khr[] = {"vkCmdBeginRenderingKHR", "vkCmdEndRenderingKHR", "vkCmdPipelineBarrier2KHR", "vkQueueSubmit2KHR"};
  const char *routes[] = {"GIPA", "GDPA", "dlsym", "linked"};
  PFN_vkVoidFunction functions[4];
  for (unsigned i = 0; i < 4; ++i) {
    const char *name = path->family == 1 ? core[i] : khr[i];
    functions[i] = path->route == 0 ? gip(instance, name) :
                   path->route == 1 ? p_vkGetDeviceProcAddr(device, name) :
                   path->route == 2 ? (PFN_vkVoidFunction)dlsym(library, name) : linked[i];
    printf("RENDER_ENTRY route=%s name=%s available=%d\n", routes[path->route], name, functions[i] != NULL);
    if (!functions[i]) return path->route >= 2 ? 3 : 2;
  }
  path->begin = (PFN_vkCmdBeginRendering)functions[0];
  path->end = (PFN_vkCmdEndRendering)functions[1];
  path->barrier = (PFN_vkCmdPipelineBarrier2)functions[2];
  path->submit = (PFN_vkQueueSubmit2)functions[3];
  return 0;
}

void render_path_begin(struct render_path *path, VkCommandBuffer cb, VkImage image,
                       VkImageView view, uint32_t width, uint32_t height) {
  VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
      .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  path->barrier(cb, &dependency);
  VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = {{0, 0, 0, 0}}}};
  VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, {width, height}}, .layerCount = 1,
      .colorAttachmentCount = 1, .pColorAttachments = &attachment};
  path->begin(cb, &rendering);
}

void render_path_end(struct render_path *path, VkCommandBuffer cb, VkImage image) {
  path->end(cb);
  VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  path->barrier(cb, &dependency);
}

void render_path_to_host(struct render_path *path, VkCommandBuffer cb) {
  VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
  VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
  path->barrier(cb, &dependency);
}

VkResult render_path_submit(struct render_path *path, VkQueue queue,
                            VkCommandBuffer cb, VkFence fence) {
  VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                      .commandBuffer = cb, .deviceMask = 1};
  VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                         .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
  return path->submit(queue, 1, &submit, fence);
}
