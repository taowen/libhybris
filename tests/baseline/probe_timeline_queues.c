#include "probe.h"

/* Two queues in the same family avoid conflating semaphore ordering with
 * queue-family ownership transfers. The consumer is submitted first. */
int timeline_queue_work(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, uint32_t family,
    PFN_vkGetSemaphoreCounterValue counter, PFN_vkWaitSemaphores wait) {
  V(vkGetDeviceQueue); V(vkGetPhysicalDeviceMemoryProperties);
  V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements);
  V(vkAllocateMemory); V(vkFreeMemory); V(vkBindBufferMemory);
  V(vkMapMemory); V(vkUnmapMemory); V(vkInvalidateMappedMemoryRanges);
  V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkResetCommandPool);
  V(vkAllocateCommandBuffers); V(vkBeginCommandBuffer); V(vkEndCommandBuffer);
  V(vkCmdFillBuffer); V(vkCmdCopyBuffer); V(vkCmdPipelineBarrier);
  V(vkCreateSemaphore); V(vkDestroySemaphore); V(vkQueueSubmit);
  V(vkCreateFence); V(vkDestroyFence); V(vkWaitForFences); V(vkResetFences);
  VkQueue queues[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  for (unsigned i = 0; i < 2; ++i) p_vkGetDeviceQueue(device, family, i, &queues[i]);
  if (!queues[0] || !queues[1] || queues[0] == queues[1]) return 2;
  printf("TIMELINE_QUEUES family=%u distinct=1\n", family);

  const VkDeviceSize bytes = 4096;
  VkPhysicalDeviceMemoryProperties memory_properties;
  p_vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
  VkBuffer buffers[2];
  VkDeviceMemory memory[2];
  VkMemoryPropertyFlags readback_flags = 0;
  for (unsigned i = 0; i < 2; ++i) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            (i ? 0 : VK_BUFFER_USAGE_TRANSFER_SRC_BIT), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(p_vkCreateBuffer(device, &bi, NULL, &buffers[i]));
    VkMemoryRequirements requirements;
    p_vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
    int index = find_mem(&memory_properties, requirements.memoryTypeBits,
        i ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!i && index < 0) index = find_mem(&memory_properties, requirements.memoryTypeBits, 0);
    /* Prefer a real non-coherent readback type when exposed. Report the
     * selected flags rather than claiming that invalidation exercised one. */
    if (i) for (uint32_t j = 0; j < memory_properties.memoryTypeCount; ++j) {
      VkMemoryPropertyFlags flags = memory_properties.memoryTypes[j].propertyFlags;
      if ((requirements.memoryTypeBits & (1u << j)) &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        index = (int)j;
        break;
      }
    }
    if (index < 0) return 2;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = (uint32_t)index};
    CHECK(p_vkAllocateMemory(device, &allocation, NULL, &memory[i]));
    CHECK(p_vkBindBufferMemory(device, buffers[i], memory[i], 0));
    if (i) readback_flags = memory_properties.memoryTypes[index].propertyFlags;
  }
  uint32_t *mapped = NULL;
  CHECK(p_vkMapMemory(device, memory[1], 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
  printf("TIMELINE_QUEUES readback-memory-flags=0x%x noncoherent=%d\n",
      readback_flags, !(readback_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
  VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = family};
  VkCommandPool pool;
  CHECK(p_vkCreateCommandPool(device, &pci, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
  VkCommandBuffer commands[2];
  CHECK(p_vkAllocateCommandBuffers(device, &cai, commands));
  VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
  VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
  VkSemaphore semaphore;
  CHECK(p_vkCreateSemaphore(device, &sci, NULL, &semaphore));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fences[2];
  for (unsigned i = 0; i < 2; ++i) CHECK(p_vkCreateFence(device, &fci, NULL, &fences[i]));
  for (uint64_t cycle = 0; cycle < 4; ++cycle) {
    uint32_t pattern = 0x193b5d7fu ^ ((uint32_t)cycle * 0x01030507u);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(p_vkBeginCommandBuffer(commands[0], &begin));
    p_vkCmdFillBuffer(commands[0], buffers[0], 0, bytes, pattern);
    CHECK(p_vkEndCommandBuffer(commands[0]));
    CHECK(p_vkBeginCommandBuffer(commands[1], &begin));
    VkBufferCopy copy = {.size = bytes};
    p_vkCmdCopyBuffer(commands[1], buffers[0], buffers[1], 1, &copy);
    VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[1], .offset = 0, .size = bytes};
    p_vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &host, 0, NULL);
    CHECK(p_vkEndCommandBuffer(commands[1]));

    uint64_t previous = cycle * 2, produced = previous + 1, consumed = previous + 2;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo timeline[2] = {
      {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
       .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &previous,
       .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &produced},
      {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
       .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &produced,
       .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &consumed}};
    VkSubmitInfo submit[2];
    for (unsigned i = 0; i < 2; ++i) submit[i] = (VkSubmitInfo){
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline[i],
      .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore, .pWaitDstStageMask = &stage,
      .commandBufferCount = 1, .pCommandBuffers = &commands[i],
      .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore};
    CHECK(p_vkQueueSubmit(queues[1], 1, &submit[1], fences[1]));
    VkResult pending = p_vkWaitForFences(device, 1, &fences[1], VK_TRUE, 0);
    if (pending != VK_TIMEOUT) {
      printf("TIMELINE_QUEUES premature-consumer=%d\n", pending);
      return 2;
    }
    CHECK(p_vkQueueSubmit(queues[0], 1, &submit[0], fences[0]));
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &consumed};
    CHECK(wait(device, &wi, 5000000000ull));
    CHECK(p_vkWaitForFences(device, 2, fences, VK_TRUE, 5000000000ull));
    uint64_t observed = UINT64_MAX;
    CHECK(counter(device, semaphore, &observed));
    if (observed != consumed) return 2;
    /* Offset zero and the entire mapped allocation satisfy non-coherent atom
     * alignment even when the allocation ends inside the final atom. */
    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory[1], .offset = 0, .size = VK_WHOLE_SIZE};
    CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &range));
    unsigned bad = 0;
    for (unsigned i = 0; i < bytes / sizeof(*mapped); ++i) if (mapped[i] != pattern) ++bad;
    printf("TIMELINE_QUEUES cycle=%llu counter=%llu pattern=0x%08x words=1024 bad=%u wait-before-signal=1\n",
        (unsigned long long)cycle, (unsigned long long)observed, pattern, bad);
    if (bad) return 2;
    CHECK(p_vkResetFences(device, 2, fences));
    CHECK(p_vkResetCommandPool(device, pool, 0));
  }
  probe_mappings("timeline-queues-complete");
  for (unsigned i = 0; i < 2; ++i) p_vkDestroyFence(device, fences[i], NULL);
  p_vkDestroySemaphore(device, semaphore, NULL);
  p_vkDestroyCommandPool(device, pool, NULL);
  p_vkUnmapMemory(device, memory[1]);
  for (unsigned i = 0; i < 2; ++i) {
    p_vkDestroyBuffer(device, buffers[i], NULL);
    p_vkFreeMemory(device, memory[i], NULL);
  }
  printf("TIMELINE_QUEUES PASS cycles=4 words=4096\n");
  return 0;
}
