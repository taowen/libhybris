#include "probe.h"

/* Partial host updates cross atom boundaries. Buffer offsets, binding offsets
 * and partial-map origins differ. The final rounds exercise both finite and
 * VK_WHOLE_SIZE ranges ending at a non-atom-aligned allocation boundary. */
static int memory_probe(int validate, int readback) {
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
  if (readback) {
    app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = app.pEngineName = "Blender";
    app.applicationVersion = app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  }
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
  V(vkBeginCommandBuffer); V(vkEndCommandBuffer); V(vkResetCommandBuffer); V(vkResetCommandPool);
  V(vkCmdCopyBuffer); V(vkCmdPipelineBarrier); V(vkQueueWaitIdle); V(vkDeviceWaitIdle); V(vkGetFenceStatus);
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
  if (readback && indices[0] != indices[1]) {
    printf("UNSUPPORTED readback shared allocation requires a common non-coherent type\n");
    rc = 3;
    goto destroy_buffers;
  }
  VkDeviceMemory memory[2];
  VkDeviceSize binding[2], allocation_size[2], mapping_offset[2] = {0, 0};
  uint8_t *mapped[2];
  for (unsigned i = 0; i < 2; ++i) {
    binding[i] = requirements[i].alignment > atom ? requirements[i].alignment : atom;
    if (binding[i] > SIZE_MAX - requirements[i].size ||
        binding[i] + requirements[i].size > SIZE_MAX - atom) return 2;
    allocation_size[i] = binding[i] + requirements[i].size + (atom > 1 ? atom / 2 : 0);
  }
  if (readback) {
    /* Keep an entire atom between the two buffers. Dirty host bytes in this
     * gap must survive completion of GPU writes to the destination. */
    VkDeviceSize alignment = requirements[1].alignment > atom ? requirements[1].alignment : atom;
    binding[1] = (binding[0] + requirements[0].size + atom + alignment - 1) & ~(alignment - 1);
    allocation_size[0] = allocation_size[1] = binding[1] + requirements[1].size + (atom > 1 ? atom / 2 : 0);
  }
  for (unsigned i = 0; i < 2; ++i) {
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = allocation_size[i], .memoryTypeIndex = (uint32_t)indices[i]};
    if (!readback || !i) CHECK(p_vkAllocateMemory(device, &allocation, NULL, &memory[i]));
    else memory[i] = memory[0];
    CHECK(p_vkBindBufferMemory(device, buffers[i], memory[i], binding[i]));
    if (!readback || !i) CHECK(p_vkMapMemory(device, memory[i], 0, VK_WHOLE_SIZE, 0, (void **)&mapped[i]));
    else mapped[i] = mapped[0];
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
  if (readback) {
    memset(mapped[1] + binding[1], 0xa5, (size_t)bytes);
    VkMappedMemoryRange clean = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[1], .offset = binding[1], .size = bytes};
    CHECK(p_vkFlushMappedMemoryRanges(device, 1, &clean));
  }
  VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
  VkCommandPool pool;
  CHECK(p_vkCreateCommandPool(device, &pci, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer command;
  CHECK(p_vkAllocateCommandBuffers(device, &cai, &command));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(p_vkCreateFence(device, &fci, NULL, &fence));
  VkFence unsignaled = VK_NULL_HANDLE;
  if (readback) CHECK(p_vkCreateFence(device, &fci, NULL, &unsignaled));
  for (unsigned cycle = 0; cycle < 7; ++cycle) {
    /* Keep the original four submissions. Then map just the buffer, followed
     * by its final half plus allocation padding. Map-relative CPU addresses
     * must not be confused with allocation-relative flush/invalidate offsets. */
    if (cycle >= 4) {
      for (unsigned i = 0; i < 2; ++i) {
        if (readback && i) {
          mapped[i] = mapped[0];
          mapping_offset[i] = mapping_offset[0];
          continue;
        }
        p_vkUnmapMemory(device, memory[i]);
        mapping_offset[i] = binding[i] + (cycle < 5 ? 0 : stride * 4);
        VkDeviceSize map_size = cycle == 4 ? bytes : allocation_size[i] - mapping_offset[i];
        if (readback) map_size = allocation_size[i] - mapping_offset[i];
        CHECK(p_vkMapMemory(device, memory[i], mapping_offset[i], map_size, 0, (void **)&mapped[i]));
        printf("MEMORY_RANGE mapping cycle=%u buffer=%u offset=%llu size=%llu allocation=%llu\n",
            cycle, i, (unsigned long long)mapping_offset[i], (unsigned long long)map_size,
            (unsigned long long)allocation_size[i]);
      }
    }
    VkBufferCopy copy = {.srcOffset = stride * (cycle < 5 ? 1 : 4),
        .dstOffset = stride * (cycle < 5 ? 2 : 4), .size = stride * 4};
    if (cycle == 0 || cycle == 5 || (readback && cycle == 6)) {
      if (readback && cycle == 6) CHECK(p_vkResetCommandPool(device, pool, 0));
      else if (cycle) CHECK(p_vkResetCommandBuffer(command, 0));
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(p_vkBeginCommandBuffer(command, &begin));
      VkMemoryBarrier upload = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
      p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &upload, 0, NULL, 0, NULL);
      p_vkCmdCopyBuffer(command, buffers[0], buffers[1], 1, &copy);
      VkMemoryBarrier readback = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
          0, 1, &readback, 0, NULL, 0, NULL);
      CHECK(p_vkEndCommandBuffer(command));
    }
    VkDeviceSize changed = stride * (cycle < 5 ? 2 : 5) + 4, length = stride + 12;
    for (size_t i = 0; i < length; ++i) expected[changed + i] = (uint8_t)(cycle * 37u + i * 19u + 3u);
    memcpy(mapped[0] + binding[0] + changed - mapping_offset[0], expected + changed, (size_t)length);
    VkDeviceSize start = (binding[0] + changed) & ~(atom - 1);
    VkDeviceSize end = (binding[0] + changed + length + atom - 1) & ~(atom - 1);
    if (cycle >= 5) end = allocation_size[0];
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[0], .offset = start, .size = (cycle == 4 || cycle == 6) ? VK_WHOLE_SIZE : end - start};
    CHECK(p_vkFlushMappedMemoryRanges(device, 1, &flush));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    if (readback) {
      volatile uint8_t *cpu = mapped[1] + binding[1] + copy.dstOffset - mapping_offset[1];
      unsigned warm = 0;
      for (size_t i = 0; i < copy.size; ++i) warm += cpu[i];
      printf("READBACK_WARM cycle=%u sum=%u\n", cycle, warm);
    }
    CHECK(p_vkQueueSubmit(queue, 1, &submit, readback && cycle == 1 ? VK_NULL_HANDLE : fence));
    VkDeviceSize dirty = binding[0] + requirements[0].size - mapping_offset[0];
    uint8_t sentinel = (uint8_t)(cycle * 31 + 9);
    if (readback) mapped[0][dirty] = sentinel;
    if (readback && cycle == 1) {
      VkResult timeout = p_vkWaitForFences(device, 1, &unsignaled, VK_TRUE, 0);
      if (timeout != VK_TIMEOUT) { printf("FAIL readback unrelated fence returned %d\n", timeout); rc = 2; }
      CHECK(p_vkQueueSubmit(queue, 0, NULL, fence));
    }
    if (readback && cycle == 2) {
      VkResult status = VK_NOT_READY;
      for (unsigned poll = 0; poll < 5000 && status == VK_NOT_READY; ++poll) {
        status = p_vkGetFenceStatus(device, fence);
        if (status == VK_NOT_READY) usleep(1000);
      }
      CHECK(status);
    }
    else if (readback && cycle == 3) CHECK(p_vkQueueWaitIdle(queue));
    else if (readback && cycle == 4) CHECK(p_vkDeviceWaitIdle(device));
    else if (readback && cycle == 5) {
      VkFence any[] = {unsignaled, fence};
      CHECK(p_vkWaitForFences(device, 2, any, VK_FALSE, 5000000000ull));
      if (p_vkGetFenceStatus(device, unsignaled) != VK_NOT_READY) rc = 2;
    }
    else CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    VkMappedMemoryRange invalidate = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[1], .offset = binding[1] + copy.dstOffset,
        .size = (cycle == 4 || cycle == 6) ? VK_WHOLE_SIZE :
                cycle < 5 ? copy.size : allocation_size[1] - binding[1] - copy.dstOffset};
    if (!readback) CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
    unsigned bad = 0;
    for (size_t i = 0; i < copy.size; ++i)
      if (mapped[1][binding[1] + copy.dstOffset + i - mapping_offset[1]] != expected[copy.srcOffset + i]) ++bad;
    printf("MEMORY_RANGE cycle=%u atom=%llu flush=%llu+%llu invalidate=%llu+%llu bytes=%llu bad=%u\n",
        cycle, (unsigned long long)atom, (unsigned long long)flush.offset, (unsigned long long)flush.size,
        (unsigned long long)invalidate.offset, (unsigned long long)invalidate.size,
        (unsigned long long)copy.size, bad);
    if (bad) rc = 2;
    if (readback) {
      unsigned dirty_bad = mapped[0][dirty] != sentinel;
      if (dirty_bad) rc = 2;
      /* Explicit invalidation is a reference after the unassisted read. This
       * distinguishes stale host data from an incorrect GPU copy. */
      CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &invalidate));
      unsigned reference_bad = 0;
      for (size_t i = 0; i < copy.size; ++i)
        if (mapped[1][binding[1] + copy.dstOffset + i - mapping_offset[1]] != expected[copy.srcOffset + i])
          ++reference_bad;
      if (reference_bad) rc = 2;
      printf("READBACK cycle=%u before_explicit_invalidate_bad=%u reference_bad=%u dirty_gap_bad=%u\n",
          cycle, bad, reference_bad, dirty_bad);
    }
    /* Identify the completed member after testing wait-any's readback. This
     * also lets validation retire that fence's command buffer before reuse. */
    if (readback && cycle == 5) CHECK(p_vkGetFenceStatus(device, fence));
    CHECK(p_vkResetFences(device, 1, &fence));
  }
  probe_mappings("memory-ranges-complete");
  p_vkDestroyFence(device, fence, NULL);
  if (unsignaled) p_vkDestroyFence(device, unsignaled, NULL);
  p_vkDestroyCommandPool(device, pool, NULL);
  free(expected);
  for (unsigned i = 0; i < (readback ? 1u : 2u); ++i) p_vkUnmapMemory(device, memory[i]);
  /* Destroy the bound buffers before freeing their allocations. */
  for (unsigned i = 0; i < 2; ++i) {
    p_vkDestroyBuffer(device, buffers[i], NULL);
    buffers[i] = VK_NULL_HANDLE;
    if (!readback) p_vkFreeMemory(device, memory[i], NULL);
  }
  if (readback) p_vkFreeMemory(device, memory[0], NULL);
destroy_buffers:
  for (unsigned i = 0; i < 2; ++i) p_vkDestroyBuffer(device, buffers[i], NULL);
  p_vkDestroyDevice(device, NULL);
  if (messenger) destroy_messenger(instance, messenger, NULL);
  p_vkDestroyInstance(instance, NULL);
  dlclose(h);
  printf("MEMORY_RANGE validation=%d automatic_readback=%d errors=%u result=%d\n", validate, readback, validation.errors, rc);
  return validation.errors ? 2 : rc;
}
int memory_ranges_probe(int validate) { return memory_probe(validate, 0); }
int memory_readback_probe(int validate) { return memory_probe(validate, 1); }
