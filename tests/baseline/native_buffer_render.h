/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_NATIVE_BUFFER_RENDER_H
#define HYBRIS_NATIVE_BUFFER_RENDER_H
#include <poll.h>
#include <errno.h>

static int native_buffer_render(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, uint32_t family, VkImageUsageFlags usage,
    uint64_t consumer, uint64_t producer,
    PFN_vkAcquireImageANDROID acquire, PFN_vkQueueSignalReleaseImageANDROID release_image) {
  if (!acquire || !release_image) return 2;
  void *gralloc = dlopen("libgralloc.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!gralloc) { printf("NATIVE_BUFFER gralloc unavailable: %s\n", dlerror()); return 2; }
  void (*initialize)(int) = sym(gralloc, "hybris_gralloc_initialize");
  int (*allocate)(int,int,int,int,buffer_handle_t *,uint32_t *) = sym(gralloc, "hybris_gralloc_allocate");
  int (*free_buffer)(buffer_handle_t,int) = sym(gralloc, "hybris_gralloc_release");
  int (*lock)(buffer_handle_t,int,int,int,int,int,void **) = sym(gralloc, "hybris_gralloc_lock");
  int (*unlock)(buffer_handle_t) = sym(gralloc, "hybris_gralloc_unlock");
  struct AHardwareBuffer *(*hardware_buffer)(buffer_handle_t) = sym(gralloc, "hybris_gralloc_get_hardware_buffer");
  int32_t (*to_legacy)(uint64_t,uint64_t) = sym(gralloc, "android_convertGralloc1To0Usage");
  void (*from_legacy)(int32_t,uint64_t *,uint64_t *) = sym(gralloc, "android_convertGralloc0To1Usage");
  if (!initialize || !allocate || !free_buffer || !lock || !unlock || !hardware_buffer || !to_legacy || !from_legacy) return 2;
  if ((consumer | producer) >> 32) { printf("NATIVE_BUFFER UNSUPPORTED allocator usage above 32 bits\n"); dlclose(gralloc); return 3; }
  initialize(0);
  /* Gralloc0 SW_READ_OFTEN=3. Retain driver-required private bits in usage. */
  int allocation_usage = to_legacy(producer, consumer) | 3;
  buffer_handle_t handle = NULL;
  uint32_t stride = 0;
  CHECK(allocate(32, 32, 1 /* HAL_PIXEL_FORMAT_RGBA_8888 */, allocation_usage, &handle, &stride));
  if (!handle || stride < 32) return 2;
  VkSwapchainImageCreateInfoANDROID swapchain_image = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID};
  VkNativeBufferANDROID native = {.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
      .pNext = &swapchain_image, .handle = handle, .stride = stride, .format = 1,
      .usage = allocation_usage, .usage3 = (uint32_t)allocation_usage, .ahb = hardware_buffer(handle)};
  from_legacy(allocation_usage, &native.usage2.producer, &native.usage2.consumer);
  printf("NATIVE_BUFFER allocation usage=0x%x stride=%u ahb=%u\n", (unsigned)allocation_usage, stride, native.ahb != NULL);
  V(vkGetDeviceQueue); V(vkGetPhysicalDeviceMemoryProperties);
  V(vkCreateImage); V(vkDestroyImage); V(vkCreateBuffer); V(vkDestroyBuffer);
  V(vkGetBufferMemoryRequirements); V(vkAllocateMemory); V(vkFreeMemory); V(vkBindBufferMemory);
  V(vkMapMemory); V(vkUnmapMemory); V(vkCreateCommandPool); V(vkDestroyCommandPool);
  V(vkAllocateCommandBuffers); V(vkBeginCommandBuffer); V(vkEndCommandBuffer); V(vkResetCommandPool);
  V(vkCmdPipelineBarrier); V(vkCmdClearColorImage); V(vkCmdCopyImageToBuffer);
  V(vkCreateSemaphore); V(vkDestroySemaphore); V(vkCreateFence); V(vkDestroyFence);
  V(vkQueueSubmit); V(vkWaitForFences);
  VkQueue queue;
  p_vkGetDeviceQueue(device, family, 0, &queue);
  VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &native,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {32,32,1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VkImage image;
  CHECK(p_vkCreateImage(device, &image_info, NULL, &image));
  /* Native-buffer creation imports its backing; no application VkDeviceMemory
   * allocation/bind is performed for this image. */
  VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 32*32*4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer readback;
  CHECK(p_vkCreateBuffer(device, &buffer_info, NULL, &readback));
  VkMemoryRequirements requirements;
  p_vkGetBufferMemoryRequirements(device, readback, &requirements);
  VkPhysicalDeviceMemoryProperties properties;
  p_vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  int type = find_mem(&properties, requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (type < 0) return 3;
  VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size, .memoryTypeIndex = type};
  VkDeviceMemory memory;
  CHECK(p_vkAllocateMemory(device, &allocation, NULL, &memory));
  CHECK(p_vkBindBufferMemory(device, readback, memory, 0));
  VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
  VkCommandPool pool;
  CHECK(p_vkCreateCommandPool(device, &pool_info, NULL, &pool));
  VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer command;
  CHECK(p_vkAllocateCommandBuffers(device, &command_info, &command));
  VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore ready, rendered;
  CHECK(p_vkCreateSemaphore(device, &semaphore_info, NULL, &ready));
  CHECK(p_vkCreateSemaphore(device, &semaphore_info, NULL, &rendered));
  int incoming = -1;
  unsigned failures = 0, native_fences = 0;
  for (unsigned round = 0; round < 4; ++round) {
    printf("NATIVE_BUFFER round=%u acquire_fd=%d\n", round, incoming);
    /* Driver owns incoming even on an acquire error; never close it again. */
    CHECK(acquire(device, image, incoming, ready, VK_NULL_HANDLE));
    incoming = -1;
    CHECK(p_vkResetCommandPool(device, pool, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(p_vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
    VkClearColorValue color = {.float32 = {round % 2, !(round % 2), round >= 2, 1}};
    p_vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {32,32,1}};
    p_vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &host, 0, NULL, 1, &barrier);
    CHECK(p_vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(p_vkCreateFence(device, &fence_info, NULL, &fence));
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ready, .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1,
        .pCommandBuffers = &command, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered};
    CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
    int released = -1;
    CHECK(release_image(queue, 1, &rendered, image, &released));
    printf("NATIVE_BUFFER round=%u release_fd=%d\n", round, released);
    if (released >= 0) {
      ++native_fences;
      if (round != 3 && (incoming = dup(released)) < 0) { close(released); return 2; }
      struct pollfd wait = {.fd = released, .events = POLLIN};
      int status;
      do { status = poll(&wait, 1, 5000); } while (status < 0 && errno == EINTR);
      close(released);
      if (status != 1 || !(wait.revents & POLLIN)) return 2;
    }
    CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    uint8_t *copied, *native_pixels;
    CHECK(p_vkMapMemory(device, memory, 0, 32*32*4, 0, (void **)&copied));
    CHECK(lock(handle, 3, 0, 0, 32, 32, (void **)&native_pixels));
    unsigned copied_bad = 0, native_bad = 0;
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
      for (unsigned component = 0; component < 4; ++component) {
        unsigned expected = color.float32[component] ? 255 : 0;
        copied_bad += copied[(y*32+x)*4+component] != expected;
        native_bad += native_pixels[(y*stride+x)*4+component] != expected;
      }
    printf("NATIVE_BUFFER round=%u copied_bad=%u native_bad=%u\n", round, copied_bad, native_bad);
    failures += !!copied_bad + !!native_bad;
    CHECK(unlock(handle));
    p_vkUnmapMemory(device, memory);
    p_vkDestroyFence(device, fence, NULL);
  }
  p_vkDestroySemaphore(device, ready, NULL); p_vkDestroySemaphore(device, rendered, NULL);
  p_vkDestroyCommandPool(device, pool, NULL);
  p_vkDestroyBuffer(device, readback, NULL); p_vkFreeMemory(device, memory, NULL);
  p_vkDestroyImage(device, image, NULL);
  CHECK(free_buffer(handle, 1));
  dlclose(gralloc);
  printf("NATIVE_BUFFER rounds=4 native_fences=%u failures=%u\n", native_fences, failures);
  return failures ? 2 : 0;
}
#endif
