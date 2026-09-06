#include "probe.h"

int groups_probe(int elf_route) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = dlsym(h, "vkGetInstanceProcAddr");
  if (!gip) return 2;
  VkInstance instance = VK_NULL_HANDLE;
  V(vkEnumerateInstanceExtensionProperties);
  uint32_t n = 0;
  CHECK(p_vkEnumerateInstanceExtensionProperties(NULL, &n, NULL));
  VkExtensionProperties *extensions = calloc(n, sizeof(*extensions));
  if (n && !extensions) return 2;
  CHECK(p_vkEnumerateInstanceExtensionProperties(NULL, &n, extensions));
  const char *extension = VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME;
  int supported = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (!strcmp(extensions[i].extensionName, extension)) supported = 1;
  free(extensions);
  if (!supported) return 3;
  V(vkCreateInstance);
  /* Separate instances: prior ordinary/core enumeration must not initialize
   * the physical handle and hide the KHR dispatch defect. */
  for (unsigned round = 0; round < 2; ++round) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = round ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &extension};
    CHECK(p_vkCreateInstance(&ci, NULL, &instance));
    V(vkDestroyInstance);
    V(vkGetPhysicalDeviceProperties);
    V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkCreateDevice);
    V(vkGetDeviceProcAddr);
    PFN_vkEnumeratePhysicalDeviceGroupsKHR query = (PFN_vkEnumeratePhysicalDeviceGroupsKHR)
        (elf_route ? dlsym(h, "vkEnumeratePhysicalDeviceGroupsKHR") :
                     (void *)gip(instance, "vkEnumeratePhysicalDeviceGroupsKHR"));
    if (!query) { p_vkDestroyInstance(instance, NULL); return 3; }
    uint32_t count = 0;
    CHECK(query(instance, &count, NULL));
    if (!count) return 2;
    VkPhysicalDeviceGroupProperties *groups = calloc(count, sizeof(*groups));
    if (!groups) return 2;
    for (uint32_t i = 0; i < count; ++i)
      groups[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
    CHECK(query(instance, &count, groups));
    for (uint32_t i = 0; i < count; ++i) {
      if (!groups[i].physicalDeviceCount) return 2;
      for (uint32_t j = 0; j < groups[i].physicalDeviceCount; ++j) {
        VkPhysicalDevice physical = groups[i].physicalDevices[j];
        printf("GROUPS api=%u route=%s physical=%p before-properties\n", round + 10,
            elf_route ? "dlsym" : "gipa", (void *)physical);
        VkPhysicalDeviceProperties properties;
        p_vkGetPhysicalDeviceProperties(physical, &properties);
        uint32_t qi;
        if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &qi)) return 2;
        float priority = 1;
        VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &priority};
        VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
        VkDevice device;
        CHECK(p_vkCreateDevice(physical, &dc, NULL, &device));
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)p_vkGetDeviceProcAddr(device, "vkDestroyDevice");
        PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)p_vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
        if (!destroy || !get_queue) return 2;
        VkQueue obtained = VK_NULL_HANDLE;
        get_queue(device, qi, 0, &obtained);
        destroy(device, NULL);
        if (!obtained) return 2;
        printf("GROUPS api=%u device=%s usable=1\n", round + 10, properties.deviceName);
      }
    }
    free(groups);
    p_vkDestroyInstance(instance, NULL);
  }
  dlclose(h);
  return 0;
}
