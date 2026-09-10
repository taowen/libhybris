#include "probe.h"

/* Versioned startup requirements, not a rendering/conformance test.
 * 5.2.1: Blender revision 9e2066aef7ef, vk_backend.cc. */
int blender_vk_probe(int modern)
{
  printf("BLENDER_VK_REQ profile=%s scope=startup-requirements\n", modern == 2 ? "5.2.1+09417042a1fa" : modern ? "5.2.1" : "4.3");
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "Blender",
                           .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                           .pEngineName = "Blender",
                           .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                           .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  VkResult created = p_vkCreateInstance(&ci, NULL, &instance);
  printf("BLENDER_VK_REQ instance_1_2=%d\n", created);
  if (created != VK_SUCCESS) {
    printf("BLENDER_VK_REQ missing=vulkan_1_2_instance\n");
    return 1;
  }
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceFeatures2);
  V(vkGetPhysicalDeviceProperties);
  V(vkEnumerateDeviceExtensionProperties);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  VkPhysicalDevice devices[8];
  if (count > 8) count = 8;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) {
    printf("BLENDER_VK_REQ missing=physical_device\n");
    p_vkDestroyInstance(instance, NULL);
    return 1;
  }
  int any = 0;
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    dynamic.pNext = &f11;
    features.pNext = &dynamic;
    p_vkGetPhysicalDeviceFeatures2(devices[i], &features);
    p_vkGetPhysicalDeviceProperties(devices[i], &props);
    uint32_t ext_count = 0;
    CHECK(p_vkEnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, NULL));
    VkExtensionProperties *ext = calloc(ext_count, sizeof(*ext));
    if (ext_count && !ext) {
      p_vkDestroyInstance(instance, NULL);
      return 2;
    }
    if (ext_count) CHECK(p_vkEnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, ext));
    int swapchain = 0, dyn_ext = 0, provoking = 0;
    for (uint32_t j = 0; j < ext_count; ++j) {
      if (!strcmp(ext[j].extensionName, "VK_EXT_provoking_vertex")) provoking = 1;
      if (!strcmp(ext[j].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchain = 1;
      if (!strcmp(ext[j].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) dyn_ext = 1;
    }
    free(ext);
    printf("BLENDER_VK_REQ device=%s geometry=%u logicOp=%u dualSrcBlend=%u "
           "imageCubeArray=%u multiDrawIndirect=%u multiViewport=%u "
           "shaderClipDistance=%u drawIndirectFirstInstance=%u "
           "fragmentStoresAndAtomics=%u dynamicRendering=%u swapchain=%d "
           "dynamic_rendering_ext=%d maxClipDistances=%u\n",
           props.deviceName, features.features.geometryShader, features.features.logicOp,
           features.features.dualSrcBlend, features.features.imageCubeArray,
           features.features.multiDrawIndirect, features.features.multiViewport,
           features.features.shaderClipDistance, features.features.drawIndirectFirstInstance,
           features.features.fragmentStoresAndAtomics, dynamic.dynamicRendering, swapchain,
           dyn_ext, props.limits.maxClipDistances);
    printf("BLENDER_VK_REQ vertexPipelineStoresAndAtomics=%u shaderDrawParameters=%u "
           "timelineSemaphore=%u bufferDeviceAddress=%u provokingVertexExtension=%d\n",
           features.features.vertexPipelineStoresAndAtomics, f11.shaderDrawParameters,
           f12.timelineSemaphore, f12.bufferDeviceAddress, provoking);
    int missing = 0;
#define NEED(bit, name) \
    do { \
      if (!(bit)) { printf("BLENDER_VK_REQ missing=%s\n", name); missing = 1; } \
    } while (0)
    NEED(features.features.geometryShader, "geometry shaders");
    NEED(features.features.logicOp, "logical operations");
    NEED(features.features.dualSrcBlend, "dual source blending");
    NEED(features.features.imageCubeArray, "image cube array");
    NEED(features.features.multiDrawIndirect, "multi draw indirect");
    NEED(features.features.multiViewport, "multi viewport");
    NEED(features.features.shaderClipDistance, "shader clip distance");
    NEED(features.features.drawIndirectFirstInstance, "draw indirect first instance");
    NEED(features.features.fragmentStoresAndAtomics, "fragment stores and atomics");
    if (modern) {
      if (modern == 1)
        NEED(features.features.vertexPipelineStoresAndAtomics, "vertex pipeline stores and atomics");
      NEED(f11.shaderDrawParameters, "shader draw parameters");
      NEED(f12.timelineSemaphore, "timeline semaphores");
      NEED(f12.bufferDeviceAddress, "buffer device address");
      NEED(provoking, "VK_EXT_provoking_vertex");
    } else {
      NEED(dynamic.dynamicRendering, "dynamic rendering");
    }
    NEED(swapchain, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    NEED(dyn_ext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
#undef NEED
    if (!missing) {
      printf("BLENDER_VK_REQ pass device=%s\n", props.deviceName);
      any = 1;
    }
  }
  p_vkDestroyInstance(instance, NULL);
  return any ? 0 : 1;
}
