#include "probe.h"

/* Execute the same transfer/readback workload through each legal entry route. */
#ifdef HYBRIS_PROBE_LINKED
#define DIRECT(name) ((PFN_vkVoidFunction)name)
#else
#define DIRECT(name) NULL
#endif
#undef V
#define RESOLVE(name, is_device) \
  PFN_##name p_##name = (PFN_##name)(linked ? DIRECT(name) : \
      dynamic ? (PFN_vkVoidFunction)dlsym(h, #name) : \
      device_route && is_device ? gdp(device, #name) : gip(instance, #name)); \
  if (!p_##name) { printf("MISSING %s via %s\n", #name, route); return 2; }
#define V(name) RESOLVE(name, 0)
#define D(name) RESOLVE(name, 1)

int vkprobe(const char *route) {
  int linked = !strcmp(route, "linked");
  int dynamic = !strcmp(route, "dlsym");
  int alias_core = !strcmp(route, "core11");
  int alias_khr = !strcmp(route, "khr11");
  int device_route = !strcmp(route, "gdpa") || alias_core || alias_khr;
#ifndef HYBRIS_PROBE_LINKED
  if (linked) return 3;
#endif
  printf("TRANSFER entry-route=%s\n", route);
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr gdp = NULL;
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  if (alias_core) {
    PFN_vkEnumerateInstanceVersion version =
        (PFN_vkEnumerateInstanceVersion)gip(NULL, "vkEnumerateInstanceVersion");
    uint32_t supported = VK_API_VERSION_1_0;
    if (version) CHECK(version(&supported));
    if (supported < VK_API_VERSION_1_1) {
      printf("UNSUPPORTED core 1.1 instance version\n");
      return 3;
    }
  }
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
                           .apiVersion = alias_core ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
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
  if (alias_core && props.apiVersion < VK_API_VERSION_1_1) {
    p_vkDestroyInstance(instance, NULL);
    printf("UNSUPPORTED core 1.1 physical device\n");
    return 3;
  }
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
  int bind2 = 0, requirements2 = 0;
  for (uint32_t i = 0; i < count; i++) {
    printf("DEVICE_EXT %s\n", ext[i].extensionName);
    bind2 |= !strcmp(ext[i].extensionName, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
    requirements2 |= !strcmp(ext[i].extensionName, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
  }
  free(ext);
  if (alias_khr && (!bind2 || !requirements2)) {
    p_vkDestroyInstance(instance, NULL);
    printf("UNSUPPORTED KHR memory2 extension pair\n");
    return 3;
  }
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
  const char *memory_extensions[] = {VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
                                     VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME};
  if (alias_khr) {
    dc.enabledExtensionCount = 2;
    dc.ppEnabledExtensionNames = memory_extensions;
  }
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  gdp = (PFN_vkGetDeviceProcAddr)gip(instance, "vkGetDeviceProcAddr");
  if (!gdp) return 2;
  PFN_vkGetBufferMemoryRequirements2 requirements_alias = NULL;
  PFN_vkBindBufferMemory2 bind_alias = NULL;
  if (alias_core || alias_khr) {
    const char *requirements_name = alias_core ? "vkGetBufferMemoryRequirements2" : "vkGetBufferMemoryRequirements2KHR";
    const char *bind_name = alias_core ? "vkBindBufferMemory2" : "vkBindBufferMemory2KHR";
    requirements_alias = (PFN_vkGetBufferMemoryRequirements2)gdp(device, requirements_name);
    bind_alias = (PFN_vkBindBufferMemory2)gdp(device, bind_name);
    if (!requirements_alias || !bind_alias) {
      printf("MISSING enabled aliases %s %s\n", requirements_name, bind_name);
      return 2;
    }
    printf("ALIAS enabled %s %s\n", requirements_name, bind_name);
  }
  D(vkGetDeviceQueue);
  D(vkCreateBuffer);
  D(vkGetBufferMemoryRequirements);
  V(vkGetPhysicalDeviceMemoryProperties);
  D(vkAllocateMemory);
  D(vkBindBufferMemory);
  D(vkMapMemory);
  D(vkUnmapMemory);
  D(vkCreateCommandPool);
  D(vkAllocateCommandBuffers);
  D(vkBeginCommandBuffer);
  D(vkCmdFillBuffer);
  D(vkCmdPipelineBarrier);
  D(vkEndCommandBuffer);
  D(vkQueueSubmit);
  D(vkCreateFence);
  D(vkWaitForFences);
  D(vkDestroyFence);
  D(vkDestroyCommandPool);
  D(vkDestroyBuffer);
  D(vkFreeMemory);
  D(vkDestroyDevice);
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkBuffer buffer;
  VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = 4096,
                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  CHECK(p_vkCreateBuffer(device, &bc, NULL, &buffer));
  VkMemoryRequirements mr;
  p_vkGetBufferMemoryRequirements(device, buffer, &mr);
  if (requirements_alias) {
    VkBufferMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buffer};
    VkMemoryRequirements2 result = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    requirements_alias(device, &info, &result);
    if (mr.size != result.memoryRequirements.size ||
        mr.alignment != result.memoryRequirements.alignment ||
        mr.memoryTypeBits != result.memoryRequirements.memoryTypeBits) {
      printf("ALIAS memory requirements mismatch\n");
      return 2;
    }
  }
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
  if (bind_alias) {
    VkBindBufferMemoryInfo info = {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
                                   .buffer = buffer, .memory = memory, .memoryOffset = 0};
    CHECK(bind_alias(device, 1, &info));
  } else {
    CHECK(p_vkBindBufferMemory(device, buffer, memory, 0));
  }
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
