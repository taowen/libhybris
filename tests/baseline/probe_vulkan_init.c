#include "probe.h"

struct vk_init_gate {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  pthread_barrier_t live_barrier;
  int start, cancel;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkCreateInstance create;
  PFN_vkEnumerateInstanceExtensionProperties enumerate;
};

struct vk_init_worker {
  struct vk_init_gate *gate;
  unsigned index, completed;
};

static int create_and_query(struct vk_init_gate *g, unsigned index) {
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "hybris-concurrent-instance", .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app};
  /* Exercise both ELF and GIPA routes from separate workers. */
  PFN_vkCreateInstance create = index & 1 ?
      (PFN_vkCreateInstance)g->gipa(NULL, "vkCreateInstance") : g->create;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult result = create ? create(&ci, NULL, &instance) : VK_ERROR_INITIALIZATION_FAILED;
  if (result != VK_SUCCESS) {
    printf("VK_INIT worker=%u create=%p result=%d\n", index, (void *)create, result);
    instance = VK_NULL_HANDLE;
  }
  PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
      (instance ? g->gipa(instance, "vkDestroyInstance") : NULL);
  PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
      (instance ? g->gipa(instance, "vkEnumeratePhysicalDevices") : NULL);
  uint32_t count = 0;
  result = enumerate ? enumerate(instance, &count, NULL) : VK_ERROR_INITIALIZATION_FAILED;
  int ok = destroy && result == VK_SUCCESS && count > 0;
  if (!ok) printf("VK_INIT worker=%u enumerate=%d count=%u destroy=%p\n",
                  index, result, count, (void *)destroy);
  /* Every worker reaches both barriers even on create/query failure.
   * Hold four successful objects live together before any is destroyed. */
  pthread_barrier_wait(&g->live_barrier);
  if (destroy) destroy(instance, NULL);
  pthread_barrier_wait(&g->live_barrier);
  return ok ? 0 : 2;
}

static int enumerate_global(struct vk_init_gate *g) {
  uint32_t count = 0;
  VkResult result = g->enumerate(NULL, &count, NULL);
  if (result != VK_SUCCESS) printf("VK_INIT global enumerate=%d\n", result);
  return result == VK_SUCCESS ? 0 : 2;
}

static void *vk_init_run(void *opaque) {
  struct vk_init_worker *w = opaque;
  struct vk_init_gate *g = w->gate;
  pthread_mutex_lock(&g->mutex);
  while (!g->start) pthread_cond_wait(&g->condition, &g->mutex);
  int cancel = g->cancel;
  pthread_mutex_unlock(&g->mutex);
  if (cancel) return NULL;
  for (unsigned i = 0; i < 4; ++i) {
    /* Some workers first enumerate; others first create. Main has done neither. */
    int first, second;
    if (w->index & 1) {
      first = enumerate_global(g);
      second = create_and_query(g, w->index);
    } else {
      first = create_and_query(g, w->index);
      second = enumerate_global(g);
    }
    if (!first && !second) ++w->completed;
  }
  return NULL;
}

int vulkan_init_probe(void) {
  void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("Vulkan dlopen: %s\n", dlerror()); return 2; }
  struct vk_init_gate g = {.mutex = PTHREAD_MUTEX_INITIALIZER,
      .condition = PTHREAD_COND_INITIALIZER,
      .gipa = dlsym(h, "vkGetInstanceProcAddr"),
      .create = dlsym(h, "vkCreateInstance"),
      .enumerate = dlsym(h, "vkEnumerateInstanceExtensionProperties")};
  if (!g.gipa || !g.create || !g.enumerate) return 2;
  if (pthread_barrier_init(&g.live_barrier, NULL, 4)) return 2;
  struct vk_init_worker workers[4] = {0};
  pthread_t threads[4];
  unsigned started = 0;
  for (; started < 4; ++started) {
    workers[started].gate = &g;
    workers[started].index = started;
    if (pthread_create(&threads[started], NULL, vk_init_run, &workers[started])) break;
  }
  pthread_mutex_lock(&g.mutex);
  g.cancel = started != 4;
  g.start = 1;
  pthread_cond_broadcast(&g.condition);
  pthread_mutex_unlock(&g.mutex);
  int rc = started == 4 ? 0 : 2;
  for (unsigned i = 0; i < started; ++i) {
    if (pthread_join(threads[i], NULL)) return 2;
    printf("VK_INIT worker=%u completed=%u expected=4\n", i, workers[i].completed);
    if (workers[i].completed != 4) rc = 2;
  }
  pthread_barrier_destroy(&g.live_barrier);
  pthread_cond_destroy(&g.condition);
  pthread_mutex_destroy(&g.mutex);
  if (dlclose(h)) rc = 2;
  printf("VK_INIT %s\n", rc ? "FAIL" : "PASS");
  return rc;
}

struct allocation_probe { unsigned live, calls, fail_after; int reject; };
struct allocation_header { void *base; size_t size; };

static void *VKAPI_CALL instance_allocate(void *user, size_t size, size_t alignment,
                                         VkSystemAllocationScope scope) {
  (void)scope;
  struct allocation_probe *p = user;
  unsigned call = __atomic_add_fetch(&p->calls, 1, __ATOMIC_RELAXED);
  if (p->reject || (p->fail_after && call > p->fail_after)) return NULL;
  if (alignment < _Alignof(struct allocation_header)) alignment = _Alignof(struct allocation_header);
  if (!alignment || (alignment & (alignment - 1)) ||
      size > SIZE_MAX - sizeof(struct allocation_header) - (alignment - 1)) return NULL;
  void *base = malloc(size + sizeof(struct allocation_header) + alignment - 1);
  if (!base) return NULL;
  uintptr_t address = ((uintptr_t)base + sizeof(struct allocation_header) + alignment - 1) & ~(alignment - 1);
  struct allocation_header *h = (struct allocation_header *)address - 1;
  h->base = base;
  h->size = size;
  __atomic_add_fetch(&p->live, 1, __ATOMIC_RELAXED);
  return (void *)address;
}

static void VKAPI_CALL instance_free(void *user, void *memory) {
  if (!memory) return;
  struct allocation_probe *p = user;
  struct allocation_header *h = (struct allocation_header *)memory - 1;
  free(h->base);
  __atomic_sub_fetch(&p->live, 1, __ATOMIC_RELAXED);
}

static void *VKAPI_CALL instance_reallocate(void *user, void *original, size_t size,
    size_t alignment, VkSystemAllocationScope scope) {
  if (!size) { instance_free(user, original); return NULL; }
  void *replacement = instance_allocate(user, size, alignment, scope);
  if (replacement && original) {
    size_t old_size = ((struct allocation_header *)original - 1)->size;
    memcpy(replacement, original, old_size < size ? old_size : size);
    instance_free(user, original);
  }
  return replacement;
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
  VkPhysicalDevice physical;
  VkResult result;
  if (route) {
    const char *name = route == 1 ? "vkEnumeratePhysicalDeviceGroups" : "vkEnumeratePhysicalDeviceGroupsKHR";
    PFN_vkEnumeratePhysicalDeviceGroups groups = (PFN_vkEnumeratePhysicalDeviceGroups)gipa(instance, name);
    VkPhysicalDeviceGroupProperties group = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
    if (!groups) return 2;
    result = groups(instance, &count, &group);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count != 1 || !group.physicalDeviceCount) return 2;
    physical = group.physicalDevices[0];
  } else {
    result = enumerate(instance, &count, &physical);
  }
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
