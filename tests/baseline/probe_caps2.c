#include "probe.h"

int caps2_probe(void) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("Vulkan dlopen: %s\n", dlerror()); return 2; }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  PFN_vkEnumerateInstanceVersion version = (PFN_vkEnumerateInstanceVersion)gip(NULL, "vkEnumerateInstanceVersion");
  uint32_t api = VK_API_VERSION_1_0;
  if (version) CHECK(version(&api));
  if (api < VK_API_VERSION_1_1) { printf("UNSUPPORTED features2 core 1.1\n"); return 3; }
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-caps2", .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkEnumeratePhysicalDevices);
  V(vkDestroyInstance);
  V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceProperties2);
  V(vkGetPhysicalDeviceFeatures);
  V(vkGetPhysicalDeviceFeatures2);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  uint32_t count = 1;
  VkPhysicalDevice pd;
  VkResult enumeration = p_vkEnumeratePhysicalDevices(instance, &count, &pd);
  if ((enumeration != VK_SUCCESS && enumeration != VK_INCOMPLETE) || !count) return 2;
  VkPhysicalDeviceProperties props;
  p_vkGetPhysicalDeviceProperties(pd, &props);
  if (props.apiVersion < VK_API_VERSION_1_1) {
    p_vkDestroyInstance(instance, NULL); return 3;
  }
  if (properties2_check(p_vkGetPhysicalDeviceProperties2, pd, &props)) return 2;
  VkPhysicalDeviceShaderDrawParametersFeatures draw = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
  VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, .pNext = &draw};
  VkPhysicalDeviceVariablePointersFeatures variable = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, .pNext = &ycbcr};
  VkPhysicalDeviceMultiviewFeatures multiview = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, .pNext = &variable};
  VkPhysicalDevice16BitStorageFeatures storage = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .pNext = &multiview};
  VkPhysicalDeviceFeatures2 chained = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &storage};
  p_vkGetPhysicalDeviceFeatures2(pd, &chained);
  if (chained.pNext != &storage || storage.pNext != &multiview || multiview.pNext != &variable ||
      variable.pNext != &ycbcr || ycbcr.pNext != &draw || draw.pNext) return 2;
  VkPhysicalDeviceFeatures features;
  p_vkGetPhysicalDeviceFeatures(pd, &features);
  int mismatch = 0;
#define CAP_UINT(name, value) do { if (strncmp(name, "features.", 9) == 0) \
    printf("CAP_VALUE %s %llu\n", name, (unsigned long long)(value)); } while (0)
#define CAP_SIGNED(name, value) ((void)0)
#define CAP_FLOAT(name, value) ((void)0)
#include "capability_fields.inc"
#undef CAP_UINT
#define FIELD(object, field) printf("CAP_VALUE features2.%s %u\n", #field, object.field)
  FIELD(storage, storageBuffer16BitAccess);
  FIELD(storage, uniformAndStorageBuffer16BitAccess);
  FIELD(storage, storagePushConstant16);
  FIELD(storage, storageInputOutput16);
  FIELD(multiview, multiview);
  FIELD(multiview, multiviewGeometryShader);
  FIELD(multiview, multiviewTessellationShader);
  FIELD(variable, variablePointersStorageBuffer);
  FIELD(variable, variablePointers);
  FIELD(ycbcr, samplerYcbcrConversion);
  FIELD(draw, shaderDrawParameters);
  /* Compare named fields, never object padding. */
#define COMPARE_FEATURE(name) do { if (features.name != chained.features.name) { \
    printf("FEATURES2 mismatch %s\n", #name); mismatch = 1; } } while (0)
#include "feature_compare.inc"
  if (mismatch) return 2;
  uint32_t qi;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, pd, &qi)) return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &chained, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  p_vkDestroyDevice(device, NULL);
  if (!features.shaderFloat64) {
    chained.features.shaderFloat64 = VK_TRUE;
    VkResult result = p_vkCreateDevice(pd, &dc, NULL, &device);
    printf("FEATURES2 unadvertised-float64 result=%d\n", result);
    if (result == VK_SUCCESS) p_vkDestroyDevice(device, NULL);
    if (result != VK_ERROR_FEATURE_NOT_PRESENT) return 2;
  }
  p_vkDestroyInstance(instance, NULL);
  printf("FEATURES2 PASS (five core 1.1 structures; query/enable, no shader execution)\n");
  return 0;
}
