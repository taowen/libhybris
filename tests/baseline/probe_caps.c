#include "probe.h"

int caps_probe(void) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-caps",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceFeatures);
  V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkGetPhysicalDeviceFormatProperties);
  V(vkEnumerateDeviceExtensionProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  V(vkGetDeviceProcAddr);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceFeatures features;
  p_vkGetPhysicalDeviceProperties(pd, &props);
  p_vkGetPhysicalDeviceFeatures(pd, &features);
  printf("CAPS gpu=%s api=%u.%u.%u maxPush=%u minUboAlign=%u BC=%u\n",
         props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
         props.limits.maxPushConstantsSize,
         (unsigned)props.limits.minUniformBufferOffsetAlignment,
         features.textureCompressionBC);
  uint32_t ext_count = 0;
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, NULL));
  VkExtensionProperties *ext = calloc(ext_count, sizeof(*ext));
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, ext));
  int has_dyn = 0, has_sync2 = 0;
  for (uint32_t i = 0; i < ext_count; i++) {
    if (!strcmp(ext[i].extensionName, "VK_KHR_dynamic_rendering"))
      has_dyn = 1;
    if (!strcmp(ext[i].extensionName, "VK_KHR_synchronization2"))
      has_sync2 = 1;
  }
  printf("CAPS ext dynamic_rendering=%d synchronization2=%d count=%u\n",
         has_dyn, has_sync2, ext_count);
  free(ext);
  VkFormatProperties fmt;
  p_vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fmt);
  printf("CAPS R8G8B8A8_UNORM linear=0x%x optimal=0x%x buffer=0x%x\n",
         fmt.linearTilingFeatures, fmt.optimalTilingFeatures,
         fmt.bufferFeatures);
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, pd, &qi))
    return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  /* Refuse a feature the query said is false. Do not strip pNext and retry. */
  if (!features.shaderFloat64) {
    VkPhysicalDeviceFeatures want = features;
    want.shaderFloat64 = VK_TRUE;
    VkDeviceCreateInfo bad = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qc,
                              .pEnabledFeatures = &want};
    VkDevice rejected = VK_NULL_HANDLE;
    VkResult br = p_vkCreateDevice(pd, &bad, NULL, &rejected);
    printf("CAPS enable-unadvertised-float64 = %d\n", br);
    if (br != VK_ERROR_FEATURE_NOT_PRESENT) {
      printf("CAPS expected VK_ERROR_FEATURE_NOT_PRESENT\n");
      if (br == VK_SUCCESS) p_vkDestroyDevice(rejected, NULL);
      return 2;
    }
  }
  const char *ghost = "VK_KHR_hybris_not_a_real_extension";
  VkDeviceCreateInfo ghost_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &qc,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = &ghost};
  VkDevice ghost_dev = VK_NULL_HANDLE;
  VkResult gr = p_vkCreateDevice(pd, &ghost_ci, NULL, &ghost_dev);
  printf("CAPS enable-unknown-extension = %d\n", gr);
  if (gr != VK_ERROR_EXTENSION_NOT_PRESENT) {
    printf("CAPS expected VK_ERROR_EXTENSION_NOT_PRESENT\n");
    if (gr == VK_SUCCESS) p_vkDestroyDevice(ghost_dev, NULL);
    return 2;
  }
  VkDeviceCreateInfo ok_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qc,
                              .pEnabledFeatures = &features};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &ok_ci, NULL, &device));
  PFN_vkGetDeviceProcAddr gdp =
      (PFN_vkGetDeviceProcAddr)gip(instance, "vkGetDeviceProcAddr");
  /* Availability is not enablement: this device enabled no extensions. */
  if (gdp(device, "vkCmdBeginRenderingKHR") != NULL) {
    printf("CAPS dynamic_rendering not enabled but GDPA present\n");
    p_vkDestroyDevice(device, NULL);
    return 2;
  }
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  printf("CAPS PASS (queries and rejection checks; no cross-backend comparison)\n");
  return 0;
}
