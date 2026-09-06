#include "probe.h"

int vkprobe(void) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkEnumerateInstanceExtensionProperties);
  uint32_t count = 0;
  CHECK(p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
  VkExtensionProperties *ext = calloc(count, sizeof(*ext));
  CHECK(p_vkEnumerateInstanceExtensionProperties(NULL, &count, ext));
  for (uint32_t i = 0; i < count; i++)
    printf("INSTANCE_EXT %s\n", ext[i].extensionName);
  free(ext);
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-baseline",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceFeatures);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkEnumerateDeviceExtensionProperties);
  V(vkDestroyInstance);
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  printf("DEVICE_COUNT %u\n", count);
  if (!count)
    return 2;
  VkPhysicalDevice devices[8];
  if (count > 8)
    count = 8;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceFeatures f;
  p_vkGetPhysicalDeviceProperties(pd, &props);
  p_vkGetPhysicalDeviceFeatures(pd, &f);
  printf("GPU %s Vulkan=%u.%u.%u driver=0x%x BC=%u ETC2=%u ASTC=%u geometry=%u "
         "tessellation=%u float64=%u int64=%u\n",
         props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
         props.driverVersion, f.textureCompressionBC, f.textureCompressionETC2,
         f.textureCompressionASTC_LDR, f.geometryShader, f.tessellationShader,
         f.shaderFloat64, f.shaderInt64);
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL));
  ext = calloc(count, sizeof(*ext));
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &count, ext));
  for (uint32_t i = 0; i < count; i++)
    printf("DEVICE_EXT %s\n", ext[i].extensionName);
  free(ext);
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
  VkQueueFamilyProperties *q = calloc(count, sizeof(*q));
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, q);
  uint32_t qi = 0;
  while (qi < count &&
         !(q[qi].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
    qi++;
  free(q);
  if (qi == count)
    return 2;
  V(vkCreateDevice);
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  V(vkGetDeviceQueue);
  V(vkCreateBuffer);
  V(vkGetBufferMemoryRequirements);
  V(vkGetPhysicalDeviceMemoryProperties);
  V(vkAllocateMemory);
  V(vkBindBufferMemory);
  V(vkMapMemory);
  V(vkUnmapMemory);
  V(vkCreateCommandPool);
  V(vkAllocateCommandBuffers);
  V(vkBeginCommandBuffer);
  V(vkCmdFillBuffer);
  V(vkCmdPipelineBarrier);
  V(vkEndCommandBuffer);
  V(vkQueueSubmit);
  V(vkCreateFence);
  V(vkWaitForFences);
  V(vkDestroyFence);
  V(vkDestroyCommandPool);
  V(vkDestroyBuffer);
  V(vkFreeMemory);
  V(vkDestroyDevice);
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkBuffer buffer;
  VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = 4096,
                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  CHECK(p_vkCreateBuffer(device, &bc, NULL, &buffer));
  VkMemoryRequirements mr;
  p_vkGetBufferMemoryRequirements(device, buffer, &mr);
  VkPhysicalDeviceMemoryProperties mp;
  p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  uint32_t mi = 0;
  for (; mi < mp.memoryTypeCount; mi++)
    if ((mr.memoryTypeBits & (1u << mi)) &&
        (mp.memoryTypes[mi].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      break;
  if (mi == mp.memoryTypeCount)
    return 3;
  VkDeviceMemory memory;
  VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = mr.size,
                             .memoryTypeIndex = mi};
  CHECK(p_vkAllocateMemory(device, &ma, NULL, &memory));
  CHECK(p_vkBindBufferMemory(device, buffer, memory, 0));
  VkCommandPool pool;
  VkCommandPoolCreateInfo pc = {.sType =
                                    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                .queueFamilyIndex = qi};
  CHECK(p_vkCreateCommandPool(device, &pc, NULL, &pool));
  VkCommandBuffer cb;
  VkCommandBufferAllocateInfo ca = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  CHECK(p_vkAllocateCommandBuffers(device, &ca, &cb));
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(p_vkBeginCommandBuffer(cb, &begin));
  p_vkCmdFillBuffer(cb, buffer, 0, 4096, 0x1234abcd);
  VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0,
                         NULL);
  CHECK(p_vkEndCommandBuffer(cb));
  VkFence fence;
  VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                     .commandBufferCount = 1,
                     .pCommandBuffers = &cb};
  CHECK(p_vkQueueSubmit(queue, 1, &si, fence));
  CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
  void *mapped;
  CHECK(p_vkMapMemory(device, memory, 0, 4096, 0, &mapped));
  int ok = 1;
  for (int i = 0; i < 1024; i++)
    if (((uint32_t *)mapped)[i] != 0x1234abcd)
      ok = 0;
  printf("GPU_FILL_READBACK %s\n", ok ? "PASS" : "FAIL");
  p_vkUnmapMemory(device, memory);
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroyCommandPool(device, pool, NULL);
  p_vkDestroyBuffer(device, buffer, NULL);
  p_vkFreeMemory(device, memory, NULL);
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  return ok ? 0 : 2;
}

