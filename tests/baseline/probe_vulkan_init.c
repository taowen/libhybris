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

struct allocation_probe { unsigned live, calls; int reject; };
struct allocation_header { void *base; size_t size; };

static void *VKAPI_CALL instance_allocate(void *user, size_t size, size_t alignment,
                                         VkSystemAllocationScope scope) {
  (void)scope;
  struct allocation_probe *p = user;
  __atomic_add_fetch(&p->calls, 1, __ATOMIC_RELAXED);
  if (p->reject) return NULL;
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

int vulkan_allocator_probe(void) {
  void *library = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) return 2;
  PFN_vkGetInstanceProcAddr gipa = dlsym(library, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
  if (!create) return 2;
  struct allocation_probe state = {0};
  VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = instance_allocate,
      .pfnReallocation = instance_reallocate, .pfnFree = instance_free};
  VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  int failed = 0;
  for (unsigned round = 0; round < 3; ++round) {
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create(&info, &callbacks, &instance);
    if (result != VK_SUCCESS) { printf("VK_ALLOC create=%d\n", result); failed = 1; break; }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    uint32_t count = 0;
    if (!enumerate || enumerate(instance, &count, NULL) != VK_SUCCESS || !count) failed = 1;
    if (!destroy) return 2;
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
