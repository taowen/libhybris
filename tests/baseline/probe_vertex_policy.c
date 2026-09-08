#include "probe.h"
#include "allocation_fixture.h"

/* Direct ICD controls distinguish adapter rejection from loader rejection. */
int vertex_policy_probe(int direct, int restricted) {
  void *h = dlopen(direct ? "libhybris-vulkan-icd.so.0" :
      (getenv("PROBE_VK") ?: "libvulkan.so.1"), RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = sym(h, direct ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
  if (!gip) return 2;
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance); V(vkEnumerateInstanceVersion);
  uint32_t api = VK_API_VERSION_1_0;
  CHECK(p_vkEnumerateInstanceVersion(&api));
  if (api < VK_API_VERSION_1_1) { dlclose(h); return 3; }
  const char *instance_extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = &instance_extension};
  struct allocation_probe allocations = {0};
  VkAllocationCallbacks callbacks = {.pUserData = &allocations, .pfnAllocation = instance_allocate,
      .pfnReallocation = instance_reallocate, .pfnFree = instance_free};
  CHECK(p_vkCreateInstance(&ci, &callbacks, &instance));
  V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkEnumerateDeviceExtensionProperties);
  V(vkGetPhysicalDeviceFeatures); V(vkGetPhysicalDeviceFeatures2); V(vkGetPhysicalDeviceFeatures2KHR);
  V(vkGetPhysicalDeviceQueueFamilyProperties); V(vkCreateDevice); V(vkGetDeviceProcAddr); V(vkDestroyDevice);
  uint32_t count = 1, family;
  VkPhysicalDevice physical;
  VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count ||
      !pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
  const char *names[] = {VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
      VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_SHADER_OBJECT_EXTENSION_NAME};
  unsigned advertised[3] = {0};
  unsigned divisor_khr = 0, divisor_ext = 0;
  CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
  VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
  if (count && !extensions) return 2;
  uint32_t total = count;
  CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
  if (count != total) return 2;
  for (uint32_t i = 0; i < count; ++i)
    for (unsigned j = 0; j < 3; ++j)
      if (!strcmp(extensions[i].extensionName, names[j])) advertised[j] = extensions[i].specVersion;
  if (total > 1) {
    count = 1;
    VkExtensionProperties prefix;
    result = p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, &prefix);
    if (result != VK_INCOMPLETE || count != 1 || strcmp(prefix.extensionName, extensions[0].extensionName) ||
        prefix.specVersion != extensions[0].specVersion) return 2;
  }
  for (uint32_t i = 0; i < total; ++i) {
    divisor_khr |= !strcmp(extensions[i].extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    divisor_ext |= !strcmp(extensions[i].extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
  }
  free(extensions);
  V(vkGetPhysicalDeviceProperties); V(vkGetPhysicalDeviceProperties2); V(vkGetPhysicalDeviceProperties2KHR);
  VkPhysicalDeviceProperties legacy_properties;
  p_vkGetPhysicalDeviceProperties(physical, &legacy_properties);
  uint32_t previous_ext = 0, previous_khr = 0;
  VkBool32 previous_first = VK_FALSE;
  for (unsigned route = 0; route < 2; ++route) {
    PFN_vkGetPhysicalDeviceProperties2 query = route ? p_vkGetPhysicalDeviceProperties2KHR : p_vkGetPhysicalDeviceProperties2;
    if (properties2_check(query, physical, &legacy_properties)) return 2;
    VkPhysicalDeviceMaintenance3Properties tail = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR khr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR, .pNext = &tail};
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT, .pNext = &khr};
    VkPhysicalDeviceProperties2 all = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ext};
    query(physical, &all);
    if (all.pNext != &ext || ext.pNext != &khr || khr.pNext != &tail || tail.pNext) return 2;
    VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR single_khr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT single_ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT};
    VkPhysicalDeviceMaintenance3Properties single_tail = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 single = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &single_khr};
    query(physical, &single); single.pNext = &single_ext; query(physical, &single);
    single.pNext = &single_tail; query(physical, &single);
    if (ext.maxVertexAttribDivisor != single_ext.maxVertexAttribDivisor ||
        khr.maxVertexAttribDivisor != single_khr.maxVertexAttribDivisor ||
        khr.supportsNonZeroFirstInstance != single_khr.supportsNonZeroFirstInstance ||
        tail.maxPerSetDescriptors != single_tail.maxPerSetDescriptors ||
        tail.maxMemoryAllocationSize != single_tail.maxMemoryAllocationSize) return 2;
    if (restricted && divisor_khr && !divisor_ext && ext.maxVertexAttribDivisor != khr.maxVertexAttribDivisor) return 2;
    if (route && (previous_ext != ext.maxVertexAttribDivisor || previous_khr != khr.maxVertexAttribDivisor ||
        previous_first != khr.supportsNonZeroFirstInstance)) return 2;
    previous_ext = ext.maxVertexAttribDivisor; previous_khr = khr.maxVertexAttribDivisor;
    previous_first = khr.supportsNonZeroFirstInstance;
    printf("VERTEX_POLICY divisor route=%u ext_advertised=%u khr_advertised=%u ext_max=%u khr_max=%u nonzero_first=%u\n",
        route, divisor_ext, divisor_khr, previous_ext, previous_khr, previous_first);
  }
  VkPhysicalDevice16BitStorageFeatures storage = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
  VkPhysicalDeviceShaderObjectFeaturesEXT object = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, .pNext = &storage};
  VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .pNext = &object};
  VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .pNext = &library};
  VkPhysicalDeviceFeatures2 chained = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vertex};
  VkBool32 core[3] = {0};
  int failed = 0;
  for (unsigned route = 0; route < 2; ++route) {
    vertex.vertexInputDynamicState = library.graphicsPipelineLibrary = object.shaderObject = VK_FALSE;
    if (route) p_vkGetPhysicalDeviceFeatures2KHR(physical, &chained);
    else p_vkGetPhysicalDeviceFeatures2(physical, &chained);
    if (chained.pNext != &vertex || vertex.pNext != &library || library.pNext != &object ||
        object.pNext != &storage || storage.pNext) return 2;
    VkBool32 values[] = {vertex.vertexInputDynamicState, library.graphicsPipelineLibrary, object.shaderObject};
    for (unsigned i = 0; i < 3; ++i) {
      printf("VERTEX_POLICY route=%u extension=%s advertised=%u feature=%u\n", route, names[i], advertised[i], values[i]);
      if (route && values[i] != core[i]) failed = 1;
      core[i] = values[i];
      if (restricted && (advertised[i] || values[i])) failed = 1;
    }
    VkPhysicalDeviceFeatures features;
    p_vkGetPhysicalDeviceFeatures(physical, &features);
    int mismatch = 0;
#define COMPARE_FEATURE(name) do { if (features.name != chained.features.name) mismatch = 1; } while (0)
#include "feature_compare.inc"
#undef COMPARE_FEATURE
    if (mismatch) failed = 1;
  }
  VkPhysicalDevice16BitStorageFeatures single_storage = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
  VkPhysicalDeviceFeatures2 single = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &single_storage};
  p_vkGetPhysicalDeviceFeatures2(physical, &single);
  if (storage.storageBuffer16BitAccess != single_storage.storageBuffer16BitAccess ||
      storage.uniformAndStorageBuffer16BitAccess != single_storage.uniformAndStorageBuffer16BitAccess ||
      storage.storagePushConstant16 != single_storage.storagePushConstant16 ||
      storage.storageInputOutput16 != single_storage.storageInputOutput16) failed = 1;
  vertex.vertexInputDynamicState = library.graphicsPipelineLibrary = object.shaderObject = VK_FALSE;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &vertex,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
  VkDevice device = VK_NULL_HANDLE;
  CHECK(p_vkCreateDevice(physical, &dc, &callbacks, &device));
  const char *commands[] = {"vkCmdSetVertexInputEXT", "vkCreateShadersEXT", "vkDestroyShaderEXT",
      "vkGetShaderBinaryDataEXT", "vkCmdBindShadersEXT"};
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    int present = p_vkGetDeviceProcAddr(device, commands[i]) != NULL;
    printf("VERTEX_POLICY command=%s present=%d\n", commands[i], present);
    if (restricted && present) failed = 1;
  }
  p_vkDestroyDevice(device, &callbacks);
  if (restricted) for (unsigned i = 0; i < 3; ++i) {
    VkBool32 *field = i == 0 ? &vertex.vertexInputDynamicState :
        i == 1 ? &library.graphicsPipelineLibrary : &object.shaderObject;
    *field = VK_TRUE;
    for (unsigned with_extension = 0; with_extension < 2; ++with_extension) {
      dc.enabledExtensionCount = with_extension;
      dc.ppEnabledExtensionNames = &names[i];
      unsigned before = allocations.calls, live = allocations.live;
      device = VK_NULL_HANDLE;
      result = p_vkCreateDevice(physical, &dc, &callbacks, &device);
      printf("VERTEX_POLICY reject extension=%s name_enabled=%u result=%d calls=%u live_delta=%d\n",
          names[i], with_extension, result, allocations.calls - before, (int)allocations.live - (int)live);
      if (result == VK_SUCCESS) p_vkDestroyDevice(device, &callbacks);
      if (result != (with_extension ? VK_ERROR_EXTENSION_NOT_PRESENT : VK_ERROR_FEATURE_NOT_PRESENT) ||
          allocations.live != live || (direct && allocations.calls != before)) failed = 1;
    }
    *field = VK_FALSE;
  }
  p_vkDestroyInstance(instance, &callbacks);
  if (allocations.live) failed = 1;
  probe_mappings("vertex-policy-complete");
  dlclose(h);
  printf("VERTEX_POLICY direct=%d restricted=%d live=%u result=%s\n", direct, restricted,
      allocations.live, failed ? "FAIL" : "PASS");
  return failed ? 2 : 0;
}
