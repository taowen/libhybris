#include "probe.h"

#ifdef HYBRIS_PROBE_LINKED
#pragma weak vkGetSemaphoreCounterValue
#pragma weak vkGetSemaphoreCounterValueKHR
#pragma weak vkWaitSemaphores
#pragma weak vkWaitSemaphoresKHR
#pragma weak vkSignalSemaphore
#pragma weak vkSignalSemaphoreKHR
#endif

struct timeline_signal_thread {
  PFN_vkSignalSemaphore signal;
  VkDevice device;
  VkSemaphore semaphore;
  pthread_barrier_t barrier;
  VkResult result;
};
static void *signal_from_thread(void *opaque) {
  struct timeline_signal_thread *state = opaque;
  pthread_barrier_wait(&state->barrier);
  usleep(20000);
  VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
      .semaphore = state->semaphore, .value = 1};
  state->result = state->signal(state->device, &info);
  return NULL;
}

int timeline_probe(int khr, int route, int validate) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) return 2;
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  uint32_t api = khr ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2, version = VK_API_VERSION_1_0;
  PFN_vkEnumerateInstanceVersion enumerate_version = (PFN_vkEnumerateInstanceVersion)gip(NULL, "vkEnumerateInstanceVersion");
  if (!enumerate_version) return 3;
  CHECK(enumerate_version(&version));
  if (version < api) { printf("UNSUPPORTED timeline instance API\n"); return 3; }
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = api};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  struct validation_state validation = {0};
  VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = validation_message, .pUserData = &validation};
  VkValidationFeaturesEXT validation_features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
      .pNext = &debug, .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *instance_extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
  if (validate) {
    ci.pNext = &validation_features;
    ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer;
    ci.enabledExtensionCount = 2; ci.ppEnabledExtensionNames = instance_extensions;
  }
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceFeatures2); V(vkEnumerateDeviceExtensionProperties);
  V(vkGetPhysicalDeviceQueueFamilyProperties); V(vkCreateDevice); V(vkDestroyDevice);
  V(vkGetDeviceProcAddr); V(vkGetDeviceQueue); V(vkQueueSubmit);
  V(vkCreateSemaphore); V(vkDestroySemaphore); V(vkCreateFence);
  V(vkDestroyFence); V(vkWaitForFences); V(vkResetFences);
  uint32_t count = 1, family;
  VkPhysicalDevice physical;
  VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count) return 2;
  VkPhysicalDeviceProperties properties;
  p_vkGetPhysicalDeviceProperties(physical, &properties);
  if (properties.apiVersion < api) { p_vkDestroyInstance(instance, NULL); return 3; }
  if (khr) {
    count = 0;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return 2;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
      found |= !strcmp(extensions[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    free(extensions);
    if (!found) { printf("UNSUPPORTED timeline extension\n"); p_vkDestroyInstance(instance, NULL); return 3; }
  }
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &timeline};
  p_vkGetPhysicalDeviceFeatures2(physical, &features);
  if (!timeline.timelineSemaphore) { p_vkDestroyInstance(instance, NULL); return 3; }
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  const char *extension = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &timeline,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc,
      .enabledExtensionCount = khr ? 1 : 0, .ppEnabledExtensionNames = khr ? &extension : NULL};
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
  PFN_vkVoidFunction linked[3] = {0};
#ifdef HYBRIS_PROBE_LINKED
  linked[0] = (PFN_vkVoidFunction)(khr ? vkGetSemaphoreCounterValueKHR : vkGetSemaphoreCounterValue);
  linked[1] = (PFN_vkVoidFunction)(khr ? vkWaitSemaphoresKHR : vkWaitSemaphores);
  linked[2] = (PFN_vkVoidFunction)(khr ? vkSignalSemaphoreKHR : vkSignalSemaphore);
#endif
  const char *core[] = {"vkGetSemaphoreCounterValue", "vkWaitSemaphores", "vkSignalSemaphore"};
  const char *aliases[] = {"vkGetSemaphoreCounterValueKHR", "vkWaitSemaphoresKHR", "vkSignalSemaphoreKHR"};
  PFN_vkVoidFunction calls[3];
  for (unsigned i = 0; i < 3; ++i) {
    const char *name = khr ? aliases[i] : core[i];
    calls[i] = route == 0 ? gip(instance, name) : route == 1 ? p_vkGetDeviceProcAddr(device, name) :
               route == 2 ? (PFN_vkVoidFunction)dlsym(h, name) : linked[i];
    printf("TIMELINE_ENTRY route=%d name=%s available=%d\n", route, name, calls[i] != NULL);
    if (!calls[i]) { p_vkDestroyDevice(device, NULL); p_vkDestroyInstance(instance, NULL); return route >= 2 ? 3 : 2; }
  }
  PFN_vkGetSemaphoreCounterValue counter = (PFN_vkGetSemaphoreCounterValue)calls[0];
  PFN_vkWaitSemaphores wait = (PFN_vkWaitSemaphores)calls[1];
  PFN_vkSignalSemaphore signal = (PFN_vkSignalSemaphore)calls[2];
  VkQueue queue;
  p_vkGetDeviceQueue(device, family, 0, &queue);
  VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
  VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
  VkSemaphore semaphore;
  CHECK(p_vkCreateSemaphore(device, &sc, NULL, &semaphore));
  VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
  struct timeline_signal_thread thread = {.signal = signal, .device = device, .semaphore = semaphore};
  if (pthread_barrier_init(&thread.barrier, NULL, 2)) return 2;
  pthread_t worker;
  if (pthread_create(&worker, NULL, signal_from_thread, &thread)) return 2;
  uint64_t one = 1;
  VkSemaphoreWaitInfo host_wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &one};
  pthread_barrier_wait(&thread.barrier);
  result = wait(device, &host_wait, 5000000000ull);
  pthread_join(worker, NULL);
  pthread_barrier_destroy(&thread.barrier);
  printf("TIMELINE threaded-host-wait=%d signal=%d\n", result, thread.result);
  if (result != VK_SUCCESS || thread.result != VK_SUCCESS) return 2;
  for (uint64_t cycle = 0; cycle < 4; ++cycle) {
    uint64_t observed = UINT64_MAX, host_value = cycle * 2 + 2, queue_value = host_value + 1;
    CHECK(counter(device, semaphore, &observed));
    if (observed != cycle * 2 + 1) return 2;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &queue_value};
    result = wait(device, &wi, 0);
    printf("TIMELINE initial-wait=%d expected=%d\n", result, VK_TIMEOUT);
    if (result != VK_TIMEOUT) return 2;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &host_value,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &queue_value};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore, .pWaitDstStageMask = &stage,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore};
    CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
    result = p_vkWaitForFences(device, 1, &fence, VK_TRUE, 0);
    if (result != VK_TIMEOUT) { printf("TIMELINE premature fence=%d\n", result); return 2; }
    VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = semaphore, .value = host_value};
    CHECK(signal(device, &si));
    CHECK(wait(device, &wi, 5000000000ull));
    CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    CHECK(counter(device, semaphore, &observed));
    if (observed != queue_value) return 2;
    CHECK(p_vkResetFences(device, 1, &fence));
    printf("TIMELINE cycle=%llu counter=%llu wait-before-signal=1\n",
        (unsigned long long)cycle, (unsigned long long)observed);
  }
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroySemaphore(device, semaphore, NULL);
  p_vkDestroyDevice(device, NULL);
  if (messenger) destroy_messenger(instance, messenger, NULL);
  p_vkDestroyInstance(instance, NULL);
  dlclose(h);
  printf("TIMELINE validation=%d errors=%u\n", validate, validation.errors);
  return validation.errors ? 2 : 0;
}
