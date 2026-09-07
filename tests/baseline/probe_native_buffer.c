/* SPDX-License-Identifier: Apache-2.0 */
#include "probe.h"
#include "native_buffer_fixture.h"
#include "native_buffer_render.h"

int native_buffer_probe(void) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkEnumerateDeviceExtensionProperties);
  V(vkGetPhysicalDeviceQueueFamilyProperties); V(vkCreateDevice); V(vkDestroyDevice); V(vkGetDeviceProcAddr);
  uint32_t count = 1, family;
  VkPhysicalDevice physical;
  VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count) return 2;
  CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
  VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
  if (!extensions) return 2;
  CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
  uint32_t version = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (!strcmp(extensions[i].extensionName, VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME)) version = extensions[i].specVersion;
  free(extensions);
  printf("NATIVE_BUFFER extension_version=%u\n", version);
  if (version < 8) {
    p_vkDestroyInstance(instance, NULL); dlclose(h);
    printf("NATIVE_BUFFER UNSUPPORTED driver-private extension\n"); return 3;
  }
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  const char *extension = VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME;
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
  VkDevice device;
  CHECK(p_vkCreateDevice(physical, &dc, NULL, &device));
#define PRIVATE(n) PFN_##n p_##n = (PFN_##n)p_vkGetDeviceProcAddr(device, #n); printf("NATIVE_BUFFER proc=%s available=%u\n", #n, p_##n != NULL)
  PRIVATE(vkGetSwapchainGrallocUsageANDROID);
  PRIVATE(vkGetSwapchainGrallocUsage2ANDROID);
  PRIVATE(vkGetSwapchainGrallocUsage3ANDROID);
  PRIVATE(vkGetSwapchainGrallocUsage4ANDROID);
  PRIVATE(vkAcquireImageANDROID);
  PRIVATE(vkQueueSignalReleaseImageANDROID);
#undef PRIVATE
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  int usage0 = 0;
  uint64_t consumer = 0, producer = 0, usage3 = 0, usage4 = 0;
  if (p_vkGetSwapchainGrallocUsageANDROID) CHECK(p_vkGetSwapchainGrallocUsageANDROID(device, VK_FORMAT_R8G8B8A8_UNORM, usage, &usage0));
  if (p_vkGetSwapchainGrallocUsage2ANDROID) CHECK(p_vkGetSwapchainGrallocUsage2ANDROID(device, VK_FORMAT_R8G8B8A8_UNORM, usage, 0, &consumer, &producer));
  VkGrallocUsageInfoANDROID u3 = {.sType = VK_STRUCTURE_TYPE_GRALLOC_USAGE_INFO_ANDROID, .format = VK_FORMAT_R8G8B8A8_UNORM, .imageUsage = usage};
  VkGrallocUsageInfo2ANDROID u4 = {.sType = VK_STRUCTURE_TYPE_GRALLOC_USAGE_INFO_2_ANDROID, .format = VK_FORMAT_R8G8B8A8_UNORM, .imageUsage = usage};
  if (version >= 9 && p_vkGetSwapchainGrallocUsage3ANDROID) CHECK(p_vkGetSwapchainGrallocUsage3ANDROID(device, &u3, &usage3));
  if (version >= 10 && p_vkGetSwapchainGrallocUsage4ANDROID) CHECK(p_vkGetSwapchainGrallocUsage4ANDROID(device, &u4, &usage4));
  printf("NATIVE_BUFFER usage0=0x%x consumer=0x%llx producer=0x%llx usage3=0x%llx usage4=0x%llx\n",
      (unsigned)usage0, (unsigned long long)consumer, (unsigned long long)producer, (unsigned long long)usage3, (unsigned long long)usage4);
  if (!p_vkGetSwapchainGrallocUsage2ANDROID) {
    printf("NATIVE_BUFFER UNSUPPORTED usage2 query\n"); return 3;
  }
  int rendered = native_buffer_render(gip, instance, physical, device, family, usage, consumer, producer,
      p_vkAcquireImageANDROID, p_vkQueueSignalReleaseImageANDROID);
  p_vkDestroyDevice(device, NULL); p_vkDestroyInstance(instance, NULL); dlclose(h);
  return rendered;
}
