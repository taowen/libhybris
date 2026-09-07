#include "probe.h"

/* Partial host updates cross atom boundaries. Buffer offsets and mapped-memory
 * offsets deliberately differ, exposing missing binding-offset adjustments. */
int memory_ranges_probe(int validate) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  struct validation_state validation = {0};
  VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = validation_message, .pUserData = &validation};
  VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
      .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
      .pNext = validate ? &vf : NULL, .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = &layer,
      .enabledExtensionCount = validate ? 2 : 0, .ppEnabledExtensionNames = extensions};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceMemoryProperties); V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice); V(vkDestroyDevice); V(vkGetDeviceQueue);
  V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements);
  V(vkAllocateMemory); V(vkFreeMemory); V(vkBindBufferMemory);
  V(vkMapMemory); V(vkUnmapMemory); V(vkFlushMappedMemoryRanges); V(vkInvalidateMappedMemoryRanges);
  V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkAllocateCommandBuffers);
  V(vkBeginCommandBuffer); V(vkEndCommandBuffer); V(vkCmdCopyBuffer); V(vkCmdPipelineBarrier);
  V(vkCreateFence); V(vkDestroyFence); V(vkWaitForFences); V(vkResetFences); V(vkQueueSubmit);
  uint32_t count = 1, family;
  VkPhysicalDevice physical;
  VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count ||
      !pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
  VkPhysicalDeviceProperties properties;
  VkPhysicalDeviceMemoryProperties memory_properties;
  p_vkGetPhysicalDeviceProperties(physical, &properties);
  p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
  VkDeviceSize atom = properties.limits.nonCoherentAtomSize;
  if (!atom || (atom & (atom - 1))) return 2;
  VkDeviceSize stride = atom < 256 ? 256 : atom;
  if (stride > SIZE_MAX / 16) return 2;
  VkDeviceSize bytes = stride * 8;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
  if (validate) {
    V(vkCreateDebugUtilsMessengerEXT);
    destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (!destroy_messenger) return 2;
    CHECK(p_vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
  }
  VkDevice device;
  CHECK(p_vkCreateDevice(physical, &dc, NULL, &device));
  VkQueue queue;
  p_vkGetDeviceQueue(device, family, 0, &queue);
  VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkMemoryRequirements requirements[2];
  int indices[2] = {-1, -1};
  int rc = 0;
  for (unsigned i = 0; i < 2; ++i) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = i ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(p_vkCreateBuffer(device, &bi, NULL, &buffers[i]));
    p_vkGetBufferMemoryRequirements(device, buffers[i], &requirements[i]);
    for (uint32_t j = 0; j < memory_properties.memoryTypeCount; ++j) {
      VkMemoryPropertyFlags flags = memory_properties.memoryTypes[j].propertyFlags;
      if ((requirements[i].memoryTypeBits & (1u << j)) &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        indices[i] = (int)j;
        break;
      }
    }
    if (indices[i] < 0) {
      printf("UNSUPPORTED memory-ranges buffer=%u no host-visible non-coherent memory\n", i);
      rc = 3;
      goto destroy_buffers;
    }
  }
  VkDeviceMemory memory[2];
  VkDeviceSize binding[2];
  uint8_t *mapped[2];
  for (unsigned i = 0; i < 2; ++i) {
    binding[i] = requirements[i].alignment > atom ? requirements[i].alignment : atom;
    if (binding[i] > SIZE_MAX - requirements[i].size) return 2;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = binding[i] + requirements[i].size, .memoryTypeIndex = (uint32_t)indices[i]};
    CHECK(p_vkAllocateMemory(device, &allocation, NULL, &memory[i]));
    CHECK(p_vkBindBufferMemory(device, buffers[i], memory[i], binding[i]));
    CHECK(p_vkMapMemory(device, memory[i], 0, VK_WHOLE_SIZE, 0, (void **)&mapped[i]));
    printf("MEMORY_RANGE buffer=%u type=%d flags=0x%x binding=%llu allocation=%llu\n", i, indices[i],
        memory_properties.memoryTypes[indices[i]].propertyFlags, (unsigned long long)binding[i],
        (unsigned long long)allocation.allocationSize);
  }
  uint8_t *expected = malloc((size_t)bytes);
  if (!expected) return 2;
  for (size_t i = 0; i < bytes; ++i) expected[i] = (uint8_t)(0xa7u ^ (i * 13u));
  memcpy(mapped[0] + binding[0], expected, (size_t)bytes);
  VkMappedMemoryRange initial = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = memory[0], .offset = binding[0], .size = bytes};
  CHECK(p_vkFlushMappedMemoryRanges(device, 1, &initial));
  VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
  VkCommandPool pool;
  CHECK(p_vkCreateCommandPool(device, &pci, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer command;
  CHECK(p_vkAllocateCommandBuffers(device, &cai, &command));
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(p_vkBeginCommandBuffer(command, &begin));
  VkMemoryBarrier upload = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
  p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &upload, 0, NULL, 0, NULL);
  VkBufferCopy copy = {.srcOffset = stride, .dstOffset = stride * 2, .size = stride * 4};
  p_vkCmdCopyBuffer(command, buffers[0], buffers[1], 1, &copy);
  VkMemoryBarrier readback = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &readback, 0, NULL, 0, NULL);
  CHECK(p_vkEndCommandBuffer(command));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(p_vkCreateFence(device, &fci, NULL, &fence));
  for (unsigned cycle = 0; cycle < 4; ++cycle) {
    VkDeviceSize changed = stride * 2 + 4, length = stride + 12;
    for (size_t i = 0; i < length; ++i) expected[changed + i] = (uint8_t)(cycle * 37u + i * 19u + 3u);
    memcpy(mapped[0] + binding[0] + changed, expected + changed, (size_t)length);
    VkDeviceSize start = (binding[0] + changed) & ~(atom - 1);
    VkDeviceSize end = (binding[0] + changed + length + atom - 1) & ~(atom - 1);
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[0], .offset = start, .size = end - start};
    CHECK(p_vkFlushMappedMemoryRanges(device, 1, &flush));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    VkMappedMemoryRange invalidate = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[1], .offset = binding[1] + copy.dstOffset, .size = copy.size};
    CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
    unsigned bad = 0;
    for (size_t i = 0; i < copy.size; ++i)
      if (mapped[1][binding[1] + copy.dstOffset + i] != expected[copy.srcOffset + i]) ++bad;
    printf("MEMORY_RANGE cycle=%u atom=%llu flush=%llu+%llu invalidate=%llu+%llu bytes=%llu bad=%u\n",
        cycle, (unsigned long long)atom, (unsigned long long)flush.offset, (unsigned long long)flush.size,
        (unsigned long long)invalidate.offset, (unsigned long long)invalidate.size,
        (unsigned long long)copy.size, bad);
    if (bad) rc = 2;
    CHECK(p_vkResetFences(device, 1, &fence));
  }
  probe_mappings("memory-ranges-complete");
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroyCommandPool(device, pool, NULL);
  free(expected);
  for (unsigned i = 0; i < 2; ++i) p_vkUnmapMemory(device, memory[i]);
  /* Destroy the bound buffers before freeing their allocations. */
  for (unsigned i = 0; i < 2; ++i) {
    p_vkDestroyBuffer(device, buffers[i], NULL);
    buffers[i] = VK_NULL_HANDLE;
    p_vkFreeMemory(device, memory[i], NULL);
  }
destroy_buffers:
  for (unsigned i = 0; i < 2; ++i) p_vkDestroyBuffer(device, buffers[i], NULL);
  p_vkDestroyDevice(device, NULL);
  if (messenger) destroy_messenger(instance, messenger, NULL);
  p_vkDestroyInstance(instance, NULL);
  dlclose(h);
  printf("MEMORY_RANGE validation=%d errors=%u result=%d\n", validate, validation.errors, rc);
  return validation.errors ? 2 : rc;
}
