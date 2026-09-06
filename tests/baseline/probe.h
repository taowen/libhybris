#ifndef HYBRIS_BASELINE_PROBE_H
#define HYBRIS_BASELINE_PROBE_H

#define _GNU_SOURCE
#ifndef HYBRIS_PROBE_LINKED
#define VK_NO_PROTOTYPES
#endif
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define E(n) __typeof__(&n) p_##n = sym(e, #n)
#define G(n) __typeof__(&n) p_##n = sym(g, #n)
#define V(n)                                                                   \
  PFN_##n p_##n = (PFN_##n)gip(instance, #n);                                  \
  if (!p_##n) {                                                                \
    printf("MISSING %s\n", #n);                                                \
    return 2;                                                                  \
  }
#define CHECK(x)                                                               \
  do {                                                                         \
    VkResult r = (x);                                                          \
    printf("%s = %d\n", #x, r);                                                \
    if (r != VK_SUCCESS)                                                       \
      return 2;                                                                \
  } while (0)

void probe_mappings(const char *phase);
void *sym(void *handle, const char *name);
int find_mem(const VkPhysicalDeviceMemoryProperties *properties,
             uint32_t bits, VkMemoryPropertyFlags need);
int pick_queue(PFN_vkGetPhysicalDeviceQueueFamilyProperties query,
               VkPhysicalDevice device, uint32_t *family);

int eglprobe(int version);
int egl_lifecycle_probe(void);
int vkprobe(const char *route);
int dispatch_probe(void);
int life_probe(int unload);
int init_probe(void);
int vulkan_init_probe(void);
int lock_init_probe(int rwlock);
int sync_destroy_probe(int check_kind);
int cond_init_probe(void);
int cond_clock_probe(void);
int shared_unavailable_probe(void);
int tls_probe(void);
int tls_bounds_probe(void);
int tls_destructor_probe(void);
int caps_probe(int check_wsi_guard);
int caps2_probe(void);
int properties2_check(PFN_vkGetPhysicalDeviceProperties2 query, VkPhysicalDevice pd,
                      const VkPhysicalDeviceProperties *legacy);
int ubo_probe(void);
int ubo_draw(int inject_wrong_binding, int validate);
int validation_probe(void);
int ubo_validation_probe(void);
struct validation_state {
  unsigned errors;
  unsigned expected;
  int injecting;
};
VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user);

#endif
