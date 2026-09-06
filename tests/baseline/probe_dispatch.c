#include "probe.h"

enum command_scope { SCOPE_GLOBAL, SCOPE_GIPA, SCOPE_INSTANCE, SCOPE_PHYSICAL, SCOPE_DEVICE };
struct dispatch_command {
  const char *name;
  enum command_scope scope;
  int core10;
  PFN_vkVoidFunction linked;
};
#ifdef HYBRIS_PROBE_LINKED
#define LINK(name) ((PFN_vkVoidFunction)name)
#else
#define LINK(name) NULL
#endif
#define ENTRY(name, scope, core, linked) { name, scope, core, linked }
static const struct dispatch_command dispatch_commands[] = {
#include "dispatch_commands.inc"
};
#undef ENTRY
#undef LINK

static int registry_queries(void *handle, PFN_vkGetInstanceProcAddr gip,
                            VkInstance instance, PFN_vkGetDeviceProcAddr gdp,
                            VkDevice device) {
  unsigned errors = 0;
  for (unsigned i = 0; i < sizeof(dispatch_commands)/sizeof(dispatch_commands[0]); ++i) {
    const struct dispatch_command *cmd = &dispatch_commands[i];
    PFN_vkVoidFunction direct = (PFN_vkVoidFunction)dlsym(handle, cmd->name);
    PFN_vkVoidFunction global = gip(VK_NULL_HANDLE, cmd->name);
    PFN_vkVoidFunction inst = gip(instance, cmd->name);
    PFN_vkVoidFunction dev = gdp(device, cmd->name);
    printf("REGISTRY_ENTRY %s dlsym=%d global=%d instance=%d device=%d linked=%d\n",
           cmd->name, !!direct, !!global, !!inst, !!dev, !!cmd->linked);
    /* The 1.0 instance requests no extensions. Later core/extension pointers
     * are observations only: presence does not permit executing the command. */
    int bad = (cmd->scope != SCOPE_GLOBAL && cmd->scope != SCOPE_GIPA && global) ||
              (cmd->scope != SCOPE_DEVICE && dev);
    if (cmd->core10) {
      bad |= !direct || (cmd->scope == SCOPE_GLOBAL ? !global : !inst) ||
             (cmd->scope == SCOPE_DEVICE && !dev);
#ifdef HYBRIS_PROBE_LINKED
      bad |= !cmd->linked;
#endif
    }
    if (bad) {
      fprintf(stderr, "REGISTRY_SCOPE FAIL %s\n", cmd->name);
      errors++;
    }
  }
  printf("REGISTRY_QUERIES commands=%zu errors=%u (resolution only)\n",
         sizeof(dispatch_commands)/sizeof(dispatch_commands[0]), errors);
  return errors ? 2 : 0;
}

static int same_or_both(void *a, void *b, const char *label) {
  printf("ENTRY %s dlsym=%p gipa=%p\n", label, a, b);
  if (!a || !b) {
    printf("ENTRY %s missing\n", label);
    return 0;
  }
  return 1;
}

int dispatch_probe(void) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = dlsym(h, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance create_dl = dlsym(h, "vkCreateInstance");
  PFN_vkEnumerateInstanceExtensionProperties enum_dl =
      dlsym(h, "vkEnumerateInstanceExtensionProperties");
  if (!gip) {
    printf("MISSING vkGetInstanceProcAddr via dlsym\n");
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip_gipa =
      (PFN_vkGetInstanceProcAddr)gip(NULL, "vkGetInstanceProcAddr");
  /* NULL-instance self lookup is optional before Vulkan 1.2. */
  (void)gip_gipa;
  PFN_vkCreateInstance create_gip =
      (PFN_vkCreateInstance)gip(NULL, "vkCreateInstance");
  PFN_vkEnumerateInstanceExtensionProperties enum_gip =
      (PFN_vkEnumerateInstanceExtensionProperties)gip(
          NULL, "vkEnumerateInstanceExtensionProperties");
  if (!same_or_both((void *)create_dl, (void *)create_gip, "vkCreateInstance") ||
      !same_or_both((void *)enum_dl, (void *)enum_gip,
                    "vkEnumerateInstanceExtensionProperties"))
    return 2;
#ifdef HYBRIS_PROBE_LINKED
  printf("LINK vkGetInstanceProcAddr=%p vkCreateInstance=%p\n",
         (void *)vkGetInstanceProcAddr, (void *)vkCreateInstance);
  if (vkGetInstanceProcAddr(NULL, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("LINK GIPA missing-symbol leaked\n");
    return 2;
  }
#endif
  if (gip(NULL, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("GIPA missing-symbol leaked\n");
    return 2;
  }
  const char *nonglobal[] = {"vkCreateDevice", "vkQueueSubmit", "vkDestroyInstance",
                            "vkCreateSwapchainKHR", "vkCreateWaylandSurfaceKHR"};
  for (unsigned i = 0; i < sizeof(nonglobal)/sizeof(nonglobal[0]); ++i) {
    if (gip(NULL, nonglobal[i])) {
      printf("GIPA_SCOPE FAIL %s without instance\n", nonglobal[i]);
      return 2;
    }
  }
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-dispatch",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  VkInstance instance = VK_NULL_HANDLE;
  CHECK(create_gip(&ci, NULL, &instance));
#ifdef HYBRIS_PROBE_LINKED
  {
    VkInstance second = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ci, NULL, &second));
    vkDestroyInstance(second, NULL);
  }
#endif
  PFN_vkEnumeratePhysicalDevices enum_pd =
      (PFN_vkEnumeratePhysicalDevices)gip(instance, "vkEnumeratePhysicalDevices");
  PFN_vkGetDeviceProcAddr gdp =
      (PFN_vkGetDeviceProcAddr)gip(instance, "vkGetDeviceProcAddr");
  PFN_vkCreateDevice create_dev =
      (PFN_vkCreateDevice)gip(instance, "vkCreateDevice");
  PFN_vkGetPhysicalDeviceQueueFamilyProperties qf =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gip(
          instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  PFN_vkDestroyInstance destroy_inst =
      (PFN_vkDestroyInstance)gip(instance, "vkDestroyInstance");
  PFN_vkDestroyDevice destroy_dev =
      (PFN_vkDestroyDevice)gip(instance, "vkDestroyDevice");
  if (!enum_pd || !gdp || !create_dev || !qf || !destroy_inst || !destroy_dev) {
    printf("MISSING instance dispatch\n");
    return 2;
  }
  if (gip(instance, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("GIPA instance missing-symbol leaked\n");
    return 2;
  }
  uint32_t count = 0;
  CHECK(enum_pd(instance, &count, NULL));
  if (!count)
    return 2;
  VkPhysicalDevice devices[8];
  if (count > 8)
    count = 8;
  CHECK(enum_pd(instance, &count, devices));
  uint32_t qcount = 0;
  qf(devices[0], &qcount, NULL);
  VkQueueFamilyProperties *q = calloc(qcount, sizeof(*q));
  qf(devices[0], &qcount, q);
  uint32_t qi = 0;
  while (qi < qcount &&
         !(q[qi].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
    qi++;
  free(q);
  if (qi == qcount)
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
  CHECK(create_dev(devices[0], &dc, NULL, &device));
  PFN_vkGetDeviceProcAddr gdp_gdpa =
      (PFN_vkGetDeviceProcAddr)gdp(device, "vkGetDeviceProcAddr");
  if (!gdp_gdpa) {
    printf("GDPA vkGetDeviceProcAddr missing\n");
    return 2;
  }
  if (gdp(device, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("GDPA missing-symbol leaked\n");
    return 2;
  }
  const char *nondevice[] = {"vkCreateInstance", "vkCreateDevice",
      "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties",
      "vkDestroySurfaceKHR", "vkCreateWaylandSurfaceKHR", "vkCreateXcbSurfaceKHR",
      "vkCreateSwapchainKHR", "vkCmdBeginRenderingKHR", "vkQueueSubmit2KHR"};
  /* No device extensions were enabled. Their commands must not be exposed.
   * Later core commands may still be returned, but must not be called. */
  for (unsigned i = 0; i < sizeof(nondevice)/sizeof(nondevice[0]); ++i) {
    if (gdp(device, nondevice[i])) {
      printf("GDPA_SCOPE FAIL %s\n", nondevice[i]);
      return 2;
    }
  }
  PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)gdp(device, "vkQueueSubmit");
  void *begin_khr = (void *)gdp(device, "vkCmdBeginRenderingKHR");
  void *begin_core = (void *)gdp(device, "vkCmdBeginRendering");
  void *submit2 = (void *)gdp(device, "vkQueueSubmit2KHR");
  if (!submit) {
    printf("GDPA vkQueueSubmit missing\n");
    return 2;
  }
  printf("GDPA vkCmdBeginRenderingKHR %s\n", begin_khr ? "present" : "null");
  printf("GDPA vkCmdBeginRendering %s\n", begin_core ? "present" : "null");
  printf("GDPA vkQueueSubmit2KHR %s\n", submit2 ? "present" : "null");
  int registry_result = registry_queries(h, gip, instance, gdp, device);
  destroy_dev(device, NULL);
  destroy_inst(instance, NULL);
  if (registry_result) return registry_result;
  printf("DISPATCH PASS\n");
  return 0;
}
