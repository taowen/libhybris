#include "probe.h"

struct properties_chain {
  VkPhysicalDeviceIDProperties id;
  VkPhysicalDeviceSubgroupProperties subgroup;
  VkPhysicalDevicePointClippingProperties clipping;
  VkPhysicalDeviceMultiviewProperties multiview;
  VkPhysicalDeviceProtectedMemoryProperties protected_memory;
  VkPhysicalDeviceMaintenance3Properties maintenance;
};

static struct properties_chain empty_chain(void) {
  return (struct properties_chain){
    .id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    .subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    .clipping.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,
    .multiview.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
    .protected_memory.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,
    .maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
}

static void id_value(const char *name, const uint8_t *bytes, size_t size) {
  printf("CAP_VALUE properties2.%s \"", name);
  for (size_t i = 0; i < size; ++i) printf("%02x", bytes[i]);
  printf("\"\n");
}

int properties2_check(PFN_vkGetPhysicalDeviceProperties2 query, VkPhysicalDevice pd,
                      const VkPhysicalDeviceProperties *legacy) {
  struct properties_chain chain = empty_chain(), single = empty_chain();
  chain.id.pNext = &chain.subgroup;
  chain.subgroup.pNext = &chain.clipping;
  chain.clipping.pNext = &chain.multiview;
  chain.multiview.pNext = &chain.protected_memory;
  chain.protected_memory.pNext = &chain.maintenance;
  VkPhysicalDeviceProperties2 result = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &chain.id};
  query(pd, &result);
  if (result.pNext != &chain.id || chain.id.pNext != &chain.subgroup ||
      chain.subgroup.pNext != &chain.clipping || chain.clipping.pNext != &chain.multiview ||
      chain.multiview.pNext != &chain.protected_memory ||
      chain.protected_memory.pNext != &chain.maintenance || chain.maintenance.pNext) return 2;
  int mismatch = 0;
#define COMPARE_PROPERTY(field) do { if (legacy->field != result.properties.field) { \
    printf("PROPERTIES2 core mismatch %s\n", #field); mismatch = 1; } } while (0)
  COMPARE_PROPERTY(apiVersion);
  COMPARE_PROPERTY(driverVersion);
  COMPARE_PROPERTY(vendorID);
  COMPARE_PROPERTY(deviceID);
  COMPARE_PROPERTY(deviceType);
#include "property_compare.inc"
#undef COMPARE_PROPERTY
  if (strncmp(legacy->deviceName, result.properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) ||
      memcmp(legacy->pipelineCacheUUID, result.properties.pipelineCacheUUID, VK_UUID_SIZE)) return 2;

  void *individual[] = {&single.id, &single.subgroup, &single.clipping,
      &single.multiview, &single.protected_memory, &single.maintenance};
  for (unsigned i = 0; i < sizeof(individual)/sizeof(individual[0]); ++i) {
    VkPhysicalDeviceProperties2 one = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = individual[i]};
    query(pd, &one);
    if (one.pNext != individual[i]) return 2;
  }
#define FIELD(object, field) do { \
    printf("CAP_VALUE properties2.%s %llu\n", #field, (unsigned long long)chain.object.field); \
    if (chain.object.field != single.object.field) { \
      printf("PROPERTIES2 chain mismatch %s\n", #field); mismatch = 1; } } while (0)
  FIELD(subgroup, subgroupSize);
  FIELD(subgroup, supportedStages);
  FIELD(subgroup, supportedOperations);
  FIELD(subgroup, quadOperationsInAllStages);
  FIELD(clipping, pointClippingBehavior);
  FIELD(multiview, maxMultiviewViewCount);
  FIELD(multiview, maxMultiviewInstanceIndex);
  FIELD(protected_memory, protectedNoFault);
  FIELD(maintenance, maxPerSetDescriptors);
  FIELD(maintenance, maxMemoryAllocationSize);
  FIELD(id, deviceLUIDValid);
  if (chain.id.deviceLUIDValid) {
    FIELD(id, deviceNodeMask);
    if (memcmp(chain.id.deviceLUID, single.id.deviceLUID, VK_LUID_SIZE)) mismatch = 1;
    id_value("deviceLUID", chain.id.deviceLUID, VK_LUID_SIZE);
  }
#undef FIELD
  if (memcmp(chain.id.deviceUUID, single.id.deviceUUID, VK_UUID_SIZE) ||
      memcmp(chain.id.driverUUID, single.id.driverUUID, VK_UUID_SIZE)) mismatch = 1;
  id_value("deviceUUID", chain.id.deviceUUID, VK_UUID_SIZE);
  id_value("driverUUID", chain.id.driverUUID, VK_UUID_SIZE);
  /* Reuse the generated named core fields for machine-readable comparison. */
  VkPhysicalDeviceProperties props = result.properties;
  VkPhysicalDeviceFeatures features = {0};
#define CAP_UINT(name, value) do { if (strncmp(name, "features.", 9)) \
    printf("CAP_VALUE properties2.%s %llu\n", name, (unsigned long long)(value)); } while (0)
#define CAP_SIGNED(name, value) printf("CAP_VALUE properties2.%s %lld\n", name, (long long)(value))
#define CAP_FLOAT(name, value) printf("CAP_VALUE properties2.%s %.9g\n", name, (double)(value))
#include "capability_fields.inc"
  printf("PROPERTIES2 %s (six core 1.1 structures, named core properties)\n", mismatch ? "FAIL" : "PASS");
  return mismatch ? 2 : 0;
}
