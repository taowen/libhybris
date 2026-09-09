#include "render_path.h"

/* Two live devices, queue2, explicit buffer free, implicit pool free and pool
 * reset. Alternate proc-query and ELF routes while recreating children. */
int render_owners_probe(int application_profile) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                          .apiVersion = application_profile ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1};
  if (application_profile) {
    app.pApplicationName = app.pEngineName = "Blender";
    app.applicationVersion = app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  }
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                            .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance); V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceQueueFamilyProperties); V(vkCreateDevice);
  V(vkDestroyDevice); V(vkGetDeviceQueue2); V(vkGetDeviceProcAddr);
  V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkResetCommandPool);
  V(vkAllocateCommandBuffers); V(vkFreeCommandBuffers);
  V(vkBeginCommandBuffer); V(vkEndCommandBuffer);
  V(vkCreateFence); V(vkDestroyFence); V(vkWaitForFences);
  V(vkQueueSubmit); V(vkCmdPipelineBarrier);
  uint32_t count = 1;
  VkPhysicalDevice physical;
  VkResult enumeration = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((enumeration != VK_SUCCESS && enumeration != VK_INCOMPLETE) || !count) return 2;
  uint32_t family;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
  struct render_path paths[2] = {{.family = 2}, {.family = 2}};
  int enabled = application_profile ? 0 : render_path_enable(&paths[0], gip, instance, physical, &dc);
  if (enabled) { p_vkDestroyInstance(instance, NULL); return enabled; }
  VkDevice devices[2];
  VkQueue queues[2];
  for (unsigned i = 0; i < 2; ++i) {
    CHECK(p_vkCreateDevice(physical, &dc, NULL, &devices[i]));
    VkDeviceQueueInfo2 qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
        .queueFamilyIndex = family, .queueIndex = 0};
    p_vkGetDeviceQueue2(devices[i], &qi, &queues[i]);
    if (!queues[i]) return 2;
  }
  for (unsigned cycle = 0; cycle < 6; ++cycle) {
    VkCommandPool pools[2];
    VkCommandBuffer commands[2][2];
    VkFence fences[2];
    for (unsigned i = 0; i < 2; ++i) {
      paths[i].route = (cycle + i) % (application_profile ? 2 : 3);
      int resolved = application_profile ? 0 : render_path_resolve(&paths[i], h, gip, instance, devices[i]);
      if (resolved) return resolved;
      VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
          .queueFamilyIndex = family};
      CHECK(p_vkCreateCommandPool(devices[i], &pc, NULL, &pools[i]));
      VkCommandBufferAllocateInfo ac = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
          .commandPool = pools[i], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
      CHECK(p_vkAllocateCommandBuffers(devices[i], &ac, commands[i]));
      p_vkFreeCommandBuffers(devices[i], pools[i], 1, &commands[i][0]);
      ac.commandBufferCount = 1;
      CHECK(p_vkAllocateCommandBuffers(devices[i], &ac, &commands[i][0]));
      CHECK(p_vkResetCommandPool(devices[i], pools[i], 0));
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      PFN_vkBeginCommandBuffer begin_command = p_vkBeginCommandBuffer;
      PFN_vkEndCommandBuffer end_command = p_vkEndCommandBuffer;
      if (application_profile && paths[i].route == 1) {
        begin_command = (PFN_vkBeginCommandBuffer)p_vkGetDeviceProcAddr(devices[i], "vkBeginCommandBuffer");
        end_command = (PFN_vkEndCommandBuffer)p_vkGetDeviceProcAddr(devices[i], "vkEndCommandBuffer");
      }
      CHECK(begin_command(commands[i][0], &begin));
      VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      if (application_profile)
        p_vkCmdPipelineBarrier(commands[i][0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
      else paths[i].barrier(commands[i][0], &dependency);
      CHECK(end_command(commands[i][0]));
      VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      CHECK(p_vkCreateFence(devices[i], &fc, NULL, &fences[i]));
      if (application_profile) {
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &commands[i][0]};
        CHECK(p_vkQueueSubmit(queues[i], 1, &submit, fences[i]));
      } else CHECK(render_path_submit(&paths[i], queues[i], commands[i][0], fences[i]));
    }
    for (unsigned j = 0; j < 2; ++j) {
      unsigned i = (cycle + j) % 2;
      CHECK(p_vkWaitForFences(devices[i], 1, &fences[i], VK_TRUE, 5000000000ull));
      p_vkDestroyFence(devices[i], fences[i], NULL);
      p_vkDestroyCommandPool(devices[i], pools[i], NULL);
    }
    printf("RENDER_OWNERS cycle=%u two_devices=1 explicit_free=1 pool_reset=1 implicit_free=1\n", cycle);
    if (application_profile && cycle == 2) {
      p_vkDestroyDevice(devices[0], NULL);
      CHECK(p_vkCreateDevice(physical, &dc, NULL, &devices[0]));
      VkDeviceQueueInfo2 qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
          .queueFamilyIndex = family, .queueIndex = 0};
      p_vkGetDeviceQueue2(devices[0], &qi, &queues[0]);
      if (!queues[0]) return 2;
      printf("RENDER_OWNERS recreated_device=0 other_device_alive=1\n");
    }
  }
  p_vkDestroyDevice(devices[0], NULL);
  p_vkDestroyDevice(devices[1], NULL);
  p_vkDestroyInstance(instance, NULL);
  dlclose(h);
  return 0;
}
