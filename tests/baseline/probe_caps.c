#include "probe.h"

int caps_probe(int check_wsi_guard) {
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
  V(vkGetPhysicalDeviceImageFormatProperties);
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
#define CAP_UINT(name, value) printf("CAP_VALUE %s %llu\n", name, (unsigned long long)(value))
#define CAP_SIGNED(name, value) printf("CAP_VALUE %s %lld\n", name, (long long)(value))
#define CAP_FLOAT(name, value) printf("CAP_VALUE %s %.9g\n", name, (double)(value))
  CAP_UINT("device.vendorID", props.vendorID);
  CAP_UINT("device.deviceID", props.deviceID);
  CAP_UINT("device.driverVersion", props.driverVersion);
  CAP_UINT("device.apiVersion", props.apiVersion);
#include "capability_fields.inc"
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
    printf("CAP_VALUE extensions.%s %u\n", ext[i].extensionName, ext[i].specVersion);
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
  const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
      VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
      VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
      VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
  for (unsigned i = 0; i < sizeof(formats)/sizeof(formats[0]); ++i) {
    p_vkGetPhysicalDeviceFormatProperties(pd, formats[i], &fmt);
    printf("CAP_VALUE formats.%d.linear %u\n", formats[i], fmt.linearTilingFeatures);
    printf("CAP_VALUE formats.%d.optimal %u\n", formats[i], fmt.optimalTilingFeatures);
    printf("CAP_VALUE formats.%d.buffer %u\n", formats[i], fmt.bufferFeatures);
    VkImageFormatProperties image = {0};
    VkResult result = p_vkGetPhysicalDeviceImageFormatProperties(pd, formats[i],
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image);
    printf("CAP_VALUE imageFormats.%d.result %d\n", formats[i], result);
    if (result != VK_SUCCESS && result != VK_ERROR_FORMAT_NOT_SUPPORTED) return 2;
    if (result == VK_SUCCESS) {
      printf("CAP_VALUE imageFormats.%d.maxWidth %u\n", formats[i], image.maxExtent.width);
      printf("CAP_VALUE imageFormats.%d.maxHeight %u\n", formats[i], image.maxExtent.height);
      printf("CAP_VALUE imageFormats.%d.maxDepth %u\n", formats[i], image.maxExtent.depth);
      printf("CAP_VALUE imageFormats.%d.maxMipLevels %u\n", formats[i], image.maxMipLevels);
      printf("CAP_VALUE imageFormats.%d.maxArrayLayers %u\n", formats[i], image.maxArrayLayers);
      printf("CAP_VALUE imageFormats.%d.sampleCounts %u\n", formats[i], image.sampleCounts);
      printf("CAP_VALUE imageFormats.%d.maxResourceSize %llu\n", formats[i],
             (unsigned long long)image.maxResourceSize);
    }
  }
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
  if (check_wsi_guard) {
    if (gdp(device, "vkCreateSwapchainKHR")) return 2;
    PFN_vkCreateSwapchainKHR create_swapchain = sym(h, "vkCreateSwapchainKHR");
    if (!create_swapchain) return 2;
    /* Intentional unsupported direct-export call: no extension/surface exists.
     * The wrapper must reject it before touching WSI or entering the driver. */
    VkSwapchainCreateInfoKHR swapchain_info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult result = create_swapchain(device, &swapchain_info, NULL, &swapchain);
    printf("WSI_DISABLED direct-create result=%d expected=%d\n", result, VK_ERROR_EXTENSION_NOT_PRESENT);
    if (result != VK_ERROR_EXTENSION_NOT_PRESENT) return 2;
  }
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  printf("CAPS PASS (queries and rejection checks; no cross-backend comparison)\n");
  return 0;
}
