#include "probe.h"

struct vk_init_gate {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
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
    return 2;
  }
  PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
      g->gipa(instance, "vkDestroyInstance");
  PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
      g->gipa(instance, "vkEnumeratePhysicalDevices");
  uint32_t count = 0;
  result = enumerate ? enumerate(instance, &count, NULL) : VK_ERROR_INITIALIZATION_FAILED;
  int ok = destroy && result == VK_SUCCESS && count > 0;
  if (!ok) printf("VK_INIT worker=%u enumerate=%d count=%u destroy=%p\n",
                  index, result, count, (void *)destroy);
  if (destroy) destroy(instance, NULL);
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
    if (w->index & 1) {
      if (enumerate_global(g) || create_and_query(g, w->index)) break;
    } else {
      if (create_and_query(g, w->index) || enumerate_global(g)) break;
    }
    ++w->completed;
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
  pthread_cond_destroy(&g.condition);
  pthread_mutex_destroy(&g.mutex);
  if (dlclose(h)) rc = 2;
  printf("VK_INIT %s\n", rc ? "FAIL" : "PASS");
  return rc;
}
