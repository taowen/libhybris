#include "probe.h"

/* Query the adapter directly: the standard loader's version describes itself,
 * and a 1.0 JSON seed prevents it from querying the driver's version at all. */
int icd_version_probe(void) {
  void *h = dlopen("libhybris-vulkan-icd.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("ICD dlopen: %s\n", dlerror()); return 2; }
  VkResult (*negotiate)(uint32_t *) = sym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
  PFN_vkGetInstanceProcAddr gip = sym(h, "vk_icdGetInstanceProcAddr");
  uint32_t interface = 5;
  if (negotiate(&interface) != VK_SUCCESS || interface != 5) return 2;
  PFN_vkEnumerateInstanceVersion query = (PFN_vkEnumerateInstanceVersion)
      gip(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  uint32_t version = 0;
  if (!query || query(&version) != VK_SUCCESS || (version >> 29) ||
      VK_VERSION_MAJOR(version) < 1) return 2;
  printf("ICD_API_VERSION %u.%u.%u\n", VK_VERSION_MAJOR(version),
         VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
  return 0;
}
