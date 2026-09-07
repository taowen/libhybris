#include "probe.h"

#include "allocation_fixture.h"

/* Exercise the same ownership registration through all three public routes. */
static VkResult enumerate_for_device(PFN_vkGetInstanceProcAddr gipa,
    VkInstance instance, unsigned route, uint32_t *count, VkPhysicalDevice *physical) {
  if (!route) {
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
        gipa(instance, "vkEnumeratePhysicalDevices");
    return enumerate ? enumerate(instance, count, physical) : VK_ERROR_INITIALIZATION_FAILED;
  }
  const char *name = route == 1 ? "vkEnumeratePhysicalDeviceGroups" : "vkEnumeratePhysicalDeviceGroupsKHR";
  PFN_vkEnumeratePhysicalDeviceGroups groups = (PFN_vkEnumeratePhysicalDeviceGroups)gipa(instance, name);
  VkPhysicalDeviceGroupProperties group = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
  if (!groups) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = groups(instance, count, &group);
  if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && *count == 1) {
    if (!group.physicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    *physical = group.physicalDevices[0];
  }
  return result;
}

static int check_device_allocator(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
    struct allocation_probe *state, const VkAllocationCallbacks *callbacks, int direct_icd, unsigned route) {
  PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
      gipa(instance, "vkEnumeratePhysicalDevices");
  PFN_vkGetPhysicalDeviceQueueFamilyProperties properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
      gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
  PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
  if (!enumerate || !properties || !create || !gdpa) return 2;
  uint32_t count = 1, qi = 0;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkResult result;
  if (direct_icd) {
    /* The caller has queried only the count, so the adapter has no physical
     * ownership record yet. Refuse callback allocations during enumeration,
     * then retry with the same instance. HAL allocation sites are not isolated
     * by this workload; retain the attempt count as evidence, not a site label. */
    unsigned live = state->live, calls = state->calls;
    state->reject = 1;
    result = enumerate_for_device(gipa, instance, route, &count, &physical);
    state->reject = 0;
    printf("VK_ALLOC physical-reject route=%u result=%d count=%u calls=%u live-delta=%d\n",
        route, result, count, state->calls - calls, (int)state->live - (int)live);
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY ||
        state->calls == calls || state->live != live) return 2;
    count = 1;
  }
  result = enumerate_for_device(gipa, instance, route, &count, &physical);
  printf("VK_ALLOC device-enumeration-route=%u\n", route);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count != 1 ||
      !pick_queue(properties, physical, &qi)) return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
  unsigned live = state->live, calls = state->calls;
  state->reject = 1;
  VkDevice device = VK_NULL_HANDLE;
  result = create(physical, &info, callbacks, &device);
  state->reject = 0;
  printf("VK_ALLOC device-reject=%d calls=%u live-delta=%d direct=%d\n", result,
      state->calls - calls, (int)state->live - (int)live, direct_icd);
  if (result == VK_SUCCESS) {
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
    if (destroy) destroy(device, callbacks);
    return 2;
  }
  if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state->live != live ||
      (direct_icd && state->calls != calls + 1)) return 2;
  result = create(physical, &info, callbacks, &device);
  if (result != VK_SUCCESS) return 2;
  PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
  PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
  if (!destroy) return 2;
  VkQueue obtained = VK_NULL_HANDLE;
  if (get_queue) get_queue(device, qi, 0, &obtained);
  destroy(device, callbacks);
  printf("VK_ALLOC device-recovered=%d live-delta=%d\n", obtained != VK_NULL_HANDLE,
      (int)state->live - (int)live);
  /* Instance destruction below checks total allocation balance; drivers may
   * retain instance-scoped caches after device destruction. */
  return obtained ? 0 : 2;
}

int vulkan_allocator_probe(int direct_icd) {
  void *library = dlopen(direct_icd ? "libhybris-vulkan-icd.so.0" :
      (getenv("PROBE_VK") ?: "libvulkan.so.1"), RTLD_NOW | RTLD_LOCAL);
  if (!library) return 2;
  PFN_vkGetInstanceProcAddr gipa = dlsym(library, direct_icd ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
  PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
  if (!create) return 2;
  struct allocation_probe state = {0};
  VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = instance_allocate,
      .pfnReallocation = instance_reallocate, .pfnFree = instance_free};
  PFN_vkEnumerateInstanceExtensionProperties extensions = (PFN_vkEnumerateInstanceExtensionProperties)
      gipa(NULL, "vkEnumerateInstanceExtensionProperties");
  uint32_t extension_count = 0;
  if (!extensions || extensions(NULL, &extension_count, NULL) != VK_SUCCESS) return 2;
  VkExtensionProperties *available = calloc(extension_count, sizeof(*available));
  if (extension_count && !available) return 2;
  if (extensions(NULL, &extension_count, available) != VK_SUCCESS) { free(available); return 2; }
  const char *group_extension = VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME;
  int has_groups = 0;
  for (uint32_t i = 0; i < extension_count; ++i)
    if (!strcmp(available[i].extensionName, group_extension)) has_groups = 1;
  free(available);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = direct_icd ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
  VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
      .enabledExtensionCount = direct_icd && has_groups ? 1 : 0, .ppEnabledExtensionNames = &group_extension};
  int failed = 0;
  if (direct_icd) {
    /* No standard loader is loaded on this path: the first allocation is
     * the adapter instance record, before the HAL creates an object. */
    state.reject = 1;
    VkInstance rejected = VK_NULL_HANDLE;
    VkResult result = create(&info, &callbacks, &rejected);
    printf("VK_ALLOC direct-initial-reject=%d calls=%u live=%u\n", result, state.calls, state.live);
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.calls != 1 || state.live) return 2;
    state.reject = 0;
    state.calls = 0;
    state.fail_after = 1;
    rejected = VK_NULL_HANDLE;
    result = create(&info, &callbacks, &rejected);
    printf("VK_ALLOC direct-hal-reject=%d calls=%u live=%u\n", result, state.calls, state.live);
    if (result == VK_SUCCESS) {
      PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(rejected, "vkDestroyInstance");
      if (destroy) destroy(rejected, &callbacks);
    }
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.calls < 2 || state.live) return 2;
    state.fail_after = 0;
  }
  for (unsigned round = 0; round < 3; ++round) {
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create(&info, &callbacks, &instance);
    if (result != VK_SUCCESS) { printf("VK_ALLOC create=%d\n", result); failed = 1; break; }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    uint32_t count = 0;
    if (!enumerate || enumerate(instance, &count, NULL) != VK_SUCCESS || !count) failed = 1;
    if (!destroy) return 2;
    if (!failed && check_device_allocator(gipa, instance, &state, &callbacks, direct_icd,
          direct_icd && (round != 2 || has_groups) ? round : 0)) failed = 1;
    destroy(instance, &callbacks);
    printf("VK_ALLOC round=%u calls=%u live=%u\n", round, state.calls, state.live);
    if (!state.calls || state.live) failed = 1;
  }
  state.reject = 1;
  VkInstance rejected = VK_NULL_HANDLE;
  VkResult result = create(&info, &callbacks, &rejected);
  printf("VK_ALLOC reject=%d expected=%d live=%u\n", result, VK_ERROR_OUT_OF_HOST_MEMORY, state.live);
  if (result == VK_SUCCESS) {
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(rejected, "vkDestroyInstance");
    if (destroy) destroy(rejected, &callbacks);
  }
  if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.live) failed = 1;
  if (dlclose(library)) failed = 1;
  printf("VK_ALLOC %s\n", failed ? "FAIL" : "PASS");
  return failed ? 2 : 0;
}

/* Frontend-owned pool/command metadata must honor callbacks and unwind a
 * partially allocated batch before invoking the driver. Independent probe. */
int command_allocator_probe(void) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  struct allocation_probe state = {0};
  VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = instance_allocate,
      .pfnReallocation = instance_reallocate, .pfnFree = instance_free};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  CHECK(p_vkCreateInstance(&ci, &callbacks, &instance));
  V(vkDestroyInstance); V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceQueueFamilyProperties); V(vkCreateDevice); V(vkDestroyDevice);
  V(vkCreateCommandPool); V(vkDestroyCommandPool);
  V(vkAllocateCommandBuffers); V(vkFreeCommandBuffers);
  uint32_t count = 1, family;
  VkPhysicalDevice physical;
  VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count ||
      !pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(p_vkCreateDevice(physical, &dc, &callbacks, &device));
  VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = family};
  unsigned live = state.live;
  state.reject = 1;
  VkCommandPool pool = VK_NULL_HANDLE;
  result = p_vkCreateCommandPool(device, &pc, &callbacks, &pool);
  state.reject = 0;
  printf("COMMAND_ALLOC pool-reject=%d live-delta=%d\n", result, (int)state.live - (int)live);
  if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.live != live) return 2;
  CHECK(p_vkCreateCommandPool(device, &pc, &callbacks, &pool));
  VkCommandBufferAllocateInfo ac = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
  VkCommandBuffer commands[3] = {
      (VkCommandBuffer)(uintptr_t)1, (VkCommandBuffer)(uintptr_t)1, (VkCommandBuffer)(uintptr_t)1};
  live = state.live;
  state.fail_after = state.calls + 1;
  result = p_vkAllocateCommandBuffers(device, &ac, commands);
  state.fail_after = 0;
  int empty = !commands[0] && !commands[1] && !commands[2];
  printf("COMMAND_ALLOC batch-reject=%d live-delta=%d all-null=%d\n",
      result, (int)state.live - (int)live, empty);
  if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.live != live || !empty) return 2;
  CHECK(p_vkAllocateCommandBuffers(device, &ac, commands));
  p_vkFreeCommandBuffers(device, pool, 1, commands);
  p_vkDestroyCommandPool(device, pool, &callbacks);
  p_vkDestroyDevice(device, &callbacks);
  p_vkDestroyInstance(instance, &callbacks);
  printf("COMMAND_ALLOC recovered=1 final-live=%u\n", state.live);
  dlclose(h);
  return state.live ? 2 : 0;
}
