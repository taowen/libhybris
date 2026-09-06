#include "probe.h"

VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
  (void)types;
  struct validation_state *state = user;
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    printf("VALIDATION %s: %s\n", data->pMessageIdName ?: "(unnamed)", data->pMessage ?: "");
    if (state->injecting && data->pMessageIdName &&
        !strcmp(data->pMessageIdName, "VUID-VkBufferCreateInfo-size-00912"))
      ++state->expected;
    else
      ++state->errors;
    /* Deliberately invalid input must not reach the driver. */
    return state->injecting ? VK_TRUE : VK_FALSE;
  }
  return VK_FALSE;
}

int validation_probe(void) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("validation loader failed: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  struct validation_state state = {0};
  VkDebugUtilsMessengerCreateInfoEXT debug = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = validation_message, .pUserData = &state};
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pNext = &debug, .pApplicationInfo = &app,
                             .enabledLayerCount = 1, .ppEnabledLayerNames = &layer,
                             .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkCreateDebugUtilsMessengerEXT);
  V(vkDestroyDebugUtilsMessengerEXT);
  VkDebugUtilsMessengerEXT messenger;
  CHECK(p_vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
  V(vkEnumeratePhysicalDevices);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  if (!count) return 2;
  VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
  if (!devices) return 2;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { free(devices); return 2; }
  VkPhysicalDevice pd = devices[0];
  free(devices);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  uint32_t family;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, pd, &family)) return 2;
  V(vkCreateDevice);
  V(vkGetDeviceProcAddr);
  float priority = 1;
  VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = family, .queueCount = 1,
                                   .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)p_vkGetDeviceProcAddr(device, "vkCreateBuffer");
  PFN_vkDestroyBuffer destroy_buffer = (PFN_vkDestroyBuffer)p_vkGetDeviceProcAddr(device, "vkDestroyBuffer");
  PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)p_vkGetDeviceProcAddr(device, "vkDestroyDevice");
  if (!create_buffer || !destroy_buffer || !destroy_device) return 2;
  VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  VkBuffer buffer;
  CHECK(create_buffer(device, &bc, NULL, &buffer));
  destroy_buffer(device, buffer, NULL);
  printf("VALIDATION legal errors=%u\n", state.errors);
  state.injecting = 1;
  bc.size = 0;
  VkResult rejected = create_buffer(device, &bc, NULL, &buffer);
  state.injecting = 0;
  if (rejected == VK_SUCCESS) destroy_buffer(device, buffer, NULL);
  probe_mappings("validation-active");
  destroy_device(device, NULL);
  p_vkDestroyDebugUtilsMessengerEXT(instance, messenger, NULL);
  p_vkDestroyInstance(instance, NULL);
  printf("VALIDATION expected=%u other-errors=%u result=%d\n",
         state.expected, state.errors, rejected);
  return state.expected == 1 && state.errors == 0 &&
         rejected == VK_ERROR_VALIDATION_FAILED_EXT ? 0 : 2;
}
