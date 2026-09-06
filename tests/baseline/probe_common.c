#include "probe.h"

void *sym(void *h, const char *n) {
  void *p = dlsym(h, n);
  if (!p) {
    printf("MISSING %s: %s\n", n, dlerror());
    exit(2);
  }
  return p;
}
int find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                    VkMemoryPropertyFlags need) {
  for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
    if ((bits & (1u << i)) &&
        (mp->memoryTypes[i].propertyFlags & need) == need)
      return (int)i;
  return -1;
}

int pick_queue(PFN_vkGetPhysicalDeviceQueueFamilyProperties qf,
                      VkPhysicalDevice pd, uint32_t *qi) {
  uint32_t count = 0;
  qf(pd, &count, NULL);
  VkQueueFamilyProperties *q = calloc(count, sizeof(*q));
  qf(pd, &count, q);
  uint32_t i = 0;
  while (i < count &&
         !(q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
    i++;
  free(q);
  if (i == count)
    return 0;
  *qi = i;
  return 1;
}


/* Diagnostic snapshots belong to this standalone probe, not the production
 * bridge. Capture file-backed mappings; they identify observed paths, not
 * arbitrary code bytes or mappings that disappeared before the snapshot. */
void probe_mappings(const char *phase) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps) {
    printf("MAPPINGS_ERROR %s\n", phase);
    return;
  }
  char line[4096];
  unsigned lines = 0;
  while (fgets(line, sizeof(line), maps)) {
    if (++lines > 4096) {
      printf("MAPPINGS_TRUNCATED %s\n", phase);
      break;
    }
    if (strchr(line, '/'))
      printf("MAPPING\t%s\t%s", phase, line);
  }
  fclose(maps);
}
