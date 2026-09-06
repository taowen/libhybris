#include "probe.h"

struct life_ctx {
  PFN_vkGetInstanceProcAddr gip;
  VkInstance instance;
  VkPhysicalDevice pd;
  uint32_t qi;
  int rc;
};

static void *life_worker(void *arg) {
  struct life_ctx *c = arg;
  PFN_vkCreateDevice create_dev =
      (PFN_vkCreateDevice)c->gip(c->instance, "vkCreateDevice");
  PFN_vkDestroyDevice destroy_dev =
      (PFN_vkDestroyDevice)c->gip(c->instance, "vkDestroyDevice");
  PFN_vkGetDeviceQueue getq =
      (PFN_vkGetDeviceQueue)c->gip(c->instance, "vkGetDeviceQueue");
  PFN_vkCreateFence create_fence =
      (PFN_vkCreateFence)c->gip(c->instance, "vkCreateFence");
  PFN_vkDestroyFence destroy_fence =
      (PFN_vkDestroyFence)c->gip(c->instance, "vkDestroyFence");
  if (!create_dev || !destroy_dev || !getq || !create_fence || !destroy_fence) {
    c->rc = 2;
    return NULL;
  }
  for (int i = 0; i < 8; i++) {
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType =
                                      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = c->qi,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qc};
    VkDevice device;
    if (create_dev(c->pd, &dc, NULL, &device) != VK_SUCCESS) {
      c->rc = 2;
      return NULL;
    }
    VkQueue queue;
    getq(device, c->qi, 0, &queue);
    VkFence fence;
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (create_fence(device, &fc, NULL, &fence) != VK_SUCCESS) {
      destroy_dev(device, NULL);
      c->rc = 2;
      return NULL;
    }
    destroy_fence(device, fence, NULL);
    destroy_dev(device, NULL);
  }
  c->rc = 0;
  return NULL;
}

int life_probe(int unload) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-life",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  if (!count)
    return 2;
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, devices[0], &qi))
    return 2;
  /* A second dlopen references the same loaded library; it does not rerun constructors. */
  void *again =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!again) {
    printf("second dlopen failed: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip2 = dlsym(again, "vkGetInstanceProcAddr");
  if (!gip2)
    return 2;
  PFN_vkDestroyInstance destroy2 =
      (PFN_vkDestroyInstance)gip2(instance, "vkDestroyInstance");
  if (!destroy2) {
    printf("second dlopen lost instance dispatch\n");
    return 2;
  }
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice a, b;
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &a));
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &b));
  if (a == b) {
    printf("two CreateDevice calls returned the same handle\n");
    return 2;
  }
  printf("DEVICES a=%p b=%p\n", (void *)a, (void *)b);
  p_vkDestroyDevice(a, NULL);
  p_vkDestroyDevice(b, NULL);
  /* Basic device recreation only; no per-object generation state is tested. */
  VkDevice c;
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &c));
  p_vkDestroyDevice(c, NULL);
  struct life_ctx workers[2] = {
      {.gip = gip, .instance = instance, .pd = devices[0], .qi = qi, .rc = 1},
      {.gip = gip, .instance = instance, .pd = devices[0], .qi = qi, .rc = 1},
  };
  pthread_t t[2];
  int started = 0;
  for (; started < 2; ++started) {
    int err = pthread_create(&t[started], NULL, life_worker, &workers[started]);
    if (err) {
      for (int j = 0; j < started; ++j) pthread_join(t[j], NULL);
      printf("pthread_create failed: %s\n", strerror(err));
      return 2;
    }
  }
  pthread_join(t[0], NULL);
  pthread_join(t[1], NULL);
  if (workers[0].rc || workers[1].rc) {
    printf("LIFE thread rc=%d %d\n", workers[0].rc, workers[1].rc);
    return 2;
  }
  p_vkDestroyInstance(instance, NULL);
  /* Recreate instance while the library remains loaded. */
  VkInstance second = VK_NULL_HANDLE;
  CHECK(p_vkCreateInstance(&ci, NULL, &second));
  p_vkDestroyInstance(second, NULL);
  if (dlclose(again) != 0) {
    printf("LIFE second reference dlclose failed: %s\n", dlerror());
    return 2;
  }
  if (unload) {
    probe_mappings("before-frontend-close");
    printf("LIFE closing final library reference; process exit is part of the check\n");
    if (dlclose(h) != 0) {
      printf("LIFE final frontend dlclose failed: %s\n", dlerror());
      return 2;
    }
  }
  /* Ordinary life mode retains the library until process exit, like other probes.
   * It does not race first hybris entry, and it does not close the frontend
   * while a worker with Android TLS is still alive. */
  printf("LIFE operations complete\n");
  return 0;
}

typedef void *(*fn_android_dlopen)(const char *, int);
typedef char *(*fn_android_dlerror)(void);
typedef void *(*fn_android_dlsym)(void *, const char *);
typedef int (*fn_android_sdk)(void);

struct init_ctx {
  pthread_barrier_t *bar;
  fn_android_dlopen adlopen;
  fn_android_dlerror adlerror;
  fn_android_dlsym adlsym;
  fn_android_sdk asdk;
  const char *lib;
  void *handle;
  void *gipa;
  int sdk;
  int rc;
};

static void *init_worker(void *arg) {
  struct init_ctx *c = arg;
  pthread_barrier_wait(c->bar);
  c->handle = c->adlopen(c->lib, RTLD_LAZY);
  if (!c->handle) {
    const char *err = c->adlerror ? c->adlerror() : NULL;
    printf("android_dlopen failed: %s\n", err ? err : "unknown");
    c->rc = 2;
    return NULL;
  }
  c->gipa = c->adlsym(c->handle, "vkGetInstanceProcAddr");
  c->sdk = c->asdk();
  if (!c->gipa || c->sdk <= 0) {
    printf("first-init incomplete handle=%p gipa=%p sdk=%d\n", c->handle,
           c->gipa, c->sdk);
    c->rc = 2;
    return NULL;
  }
  c->rc = 0;
  return NULL;
}

int init_probe(void) {
  /* Two threads' first android_dlopen. libvulkan.so.1 constructors are
   * serialized by glibc, so this loads common and calls android_* directly. */
  void *common =
      dlopen(getenv("PROBE_COMMON") ?: "libhybris-common.so.1",
             RTLD_NOW | RTLD_LOCAL);
  if (!common) {
    printf("INIT libhybris-common load failed: %s\n", dlerror());
    return 2;
  }
  fn_android_dlopen adlopen = dlsym(common, "android_dlopen");
  fn_android_dlerror adlerror = dlsym(common, "android_dlerror");
  fn_android_dlsym adlsym = dlsym(common, "android_dlsym");
  fn_android_sdk asdk = dlsym(common, "android_get_application_target_sdk_version");
  if (!adlopen || !adlsym || !asdk) {
    printf("MISSING android_dlopen/android_dlsym/android_get_application_target_sdk_version\n");
    return 2;
  }
  const char *lib = getenv("LIBVULKAN") ? getenv("LIBVULKAN") : "libvulkan.so";
  pthread_barrier_t bar;
  if (pthread_barrier_init(&bar, NULL, 2)) {
    printf("pthread_barrier_init failed\n");
    return 2;
  }
  struct init_ctx workers[2] = {
      {.bar = &bar,
       .adlopen = adlopen,
       .adlerror = adlerror,
       .adlsym = adlsym,
       .asdk = asdk,
       .lib = lib,
       .rc = 1},
      {.bar = &bar,
       .adlopen = adlopen,
       .adlerror = adlerror,
       .adlsym = adlsym,
       .asdk = asdk,
       .lib = lib,
       .rc = 1},
  };
  pthread_t t[2];
  int started = 0;
  for (; started < 2; ++started) {
    int err = pthread_create(&t[started], NULL, init_worker, &workers[started]);
    if (err) {
      /* With one worker, main must supply the missing barrier participant
       * before joining it. The probe still fails after that worker exits. */
      if (started == 1) pthread_barrier_wait(&bar);
      for (int j = 0; j < started; ++j) pthread_join(t[j], NULL);
      pthread_barrier_destroy(&bar);
      printf("pthread_create failed: %s\n", strerror(err));
      return 2;
    }
  }
  pthread_join(t[0], NULL);
  pthread_join(t[1], NULL);
  pthread_barrier_destroy(&bar);
  if (workers[0].rc || workers[1].rc) {
    printf("INIT thread rc=%d %d\n", workers[0].rc, workers[1].rc);
    return 2;
  }
  if (workers[0].sdk != workers[1].sdk) {
    printf("INIT sdk mismatch %d %d\n", workers[0].sdk, workers[1].sdk);
    return 2;
  }
  /* Both threads resolved the entry after concurrent first calls. This probe
   * does not count init invocations or require equal handles.
   * Vendor libraries stay mapped; this does not android_dlclose them. */
  printf("INIT a=%p gipa=%p b=%p gipa=%p sdk=%d\n", workers[0].handle,
         workers[0].gipa, workers[1].handle, workers[1].gipa, workers[0].sdk);
  printf("INIT concurrent first android_dlopen complete\n");
  return 0;
}

struct tls_ctx {
  void *frontend;
  pthread_barrier_t *ready;
  pthread_barrier_t *closed;
  int rc;
};

static int tls_use_frontend(void *h) {
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-tls",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  V(vkCreateFence);
  V(vkDestroyFence);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  if (!count)
    return 2;
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, devices[0], &qi))
    return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &device));
  VkFence fence;
  VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  return 0;
}

static void *tls_worker(void *arg) {
  struct tls_ctx *c = arg;
  c->rc = tls_use_frontend(c->frontend);
  pthread_barrier_wait(c->ready);
  pthread_barrier_wait(c->closed);
  printf("TLS worker returning after frontend close\n");
  return NULL;
}

int tls_probe(void) {
  /* Worker exercises Vulkan on its own thread. Main then drops the
   * last frontend reference. Worker exit is the TLS-cleanup boundary.
   * dlclose returning 0 is not unmap proof; process exit is not reclaim. */
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  pthread_barrier_t ready, closed;
  if (pthread_barrier_init(&ready, NULL, 2)) {
    printf("pthread_barrier_init failed\n");
    return 2;
  }
  if (pthread_barrier_init(&closed, NULL, 2)) {
    pthread_barrier_destroy(&ready);
    printf("pthread_barrier_init failed\n");
    return 2;
  }
  struct tls_ctx ctx = {
      .frontend = h, .ready = &ready, .closed = &closed, .rc = 1};
  pthread_t t;
  int err = pthread_create(&t, NULL, tls_worker, &ctx);
  if (err) {
    pthread_barrier_destroy(&ready);
    pthread_barrier_destroy(&closed);
    printf("pthread_create failed: %s\n", strerror(err));
    return 2;
  }
  pthread_barrier_wait(&ready);
  int dc = -1;
  if (ctx.rc == 0) {
    probe_mappings("before-frontend-close");
    dc = dlclose(h);
    printf("TLS frontend dlclose=%d\n", dc);
  } else {
    printf("TLS frontend retained after failed Vulkan setup/teardown\n");
  }
  pthread_barrier_wait(&closed);
  pthread_join(t, NULL);
  pthread_barrier_destroy(&ready);
  pthread_barrier_destroy(&closed);
  if (ctx.rc) {
    printf("TLS worker rc=%d\n", ctx.rc);
    return ctx.rc;
  }
  if (dc != 0) {
    printf("TLS frontend dlclose failed\n");
    return 2;
  }
  printf("TLS worker joined after frontend close (TLS allocation/destructors not instrumented)\n");
  return 0;
}

