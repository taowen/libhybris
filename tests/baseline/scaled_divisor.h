/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SCALED_DIVISOR_H
#define HYBRIS_SCALED_DIVISOR_H
/* Keep capability negotiation and instance data separate from rendering setup. */
static int scaled_divisor_features(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkPhysicalDevice pd, unsigned mode, VkDeviceCreateInfo *dc,
    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR *features, const char **extension) {
  PFN_vkEnumerateDeviceExtensionProperties enumerate = (PFN_vkEnumerateDeviceExtensionProperties)
      gip(instance, "vkEnumerateDeviceExtensionProperties");
  PFN_vkGetPhysicalDeviceFeatures2 get_features = (PFN_vkGetPhysicalDeviceFeatures2)
      gip(instance, "vkGetPhysicalDeviceFeatures2");
  PFN_vkGetPhysicalDeviceProperties2 get_properties = (PFN_vkGetPhysicalDeviceProperties2)
      gip(instance, "vkGetPhysicalDeviceProperties2");
  if (!enumerate || !get_features || !get_properties) return 2;
  uint32_t count = 0;
  CHECK(enumerate(pd, NULL, &count, NULL));
  VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
  if (!extensions) return 2;
  VkResult result = enumerate(pd, NULL, &count, extensions);
  unsigned khr = 0, ext = 0;
  if (result == VK_SUCCESS) for (unsigned i = 0; i < count; ++i) {
    if (!strcmp(extensions[i].extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) khr = extensions[i].specVersion;
    if (!strcmp(extensions[i].extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) ext = extensions[i].specVersion;
  }
  free(extensions);
  CHECK(result);
  if (!khr && ext < 3) {
    printf("SCALED DIVISOR UNSUPPORTED extension khr=%u ext=%u\n", khr, ext);
    return 3;
  }
  *extension = khr ? VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
  features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR;
  VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = features};
  get_features(pd, &query);
  VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR kp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
  VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT ep = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT};
  VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = khr ? (void *)&kp : (void *)&ep};
  get_properties(pd, &properties);
  unsigned max_divisor = khr ? kp.maxVertexAttribDivisor : ep.maxVertexAttribDivisor;
  unsigned nonzero_first = khr ? kp.supportsNonZeroFirstInstance : VK_TRUE;
  printf("SCALED DIVISOR extension=%s max=%u divisor=%u zero=%u nonzero_first=%u mode=%u\n",
      *extension, max_divisor, features->vertexAttributeInstanceRateDivisor,
      features->vertexAttributeInstanceRateZeroDivisor, nonzero_first, mode);
  if (!features->vertexAttributeInstanceRateDivisor || (!(mode & 2) && max_divisor < 3) ||
      ((mode & 2) && !features->vertexAttributeInstanceRateZeroDivisor) || ((mode & 4) && !nonzero_first)) {
    printf("SCALED DIVISOR UNSUPPORTED requested combination\n");
    return 3;
  }
  features->vertexAttributeInstanceRateZeroDivisor = !!(mode & 2);
  dc->pNext = features;
  dc->enabledExtensionCount = 1;
  dc->ppEnabledExtensionNames = extension;
  return 0;
}

/* Negotiate the command-time binding stride separately from dynamic vertex input. */
static int scaled_stride_features(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkPhysicalDevice pd, VkDeviceCreateInfo *dc,
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *features, const char **extensions) {
  PFN_vkEnumerateDeviceExtensionProperties enumerate = (void *)gip(instance, "vkEnumerateDeviceExtensionProperties");
  PFN_vkGetPhysicalDeviceFeatures2 query = (void *)gip(instance, "vkGetPhysicalDeviceFeatures2");
  uint32_t count = 0;
  CHECK(enumerate(pd, NULL, &count, NULL));
  VkExtensionProperties *available = calloc(count, sizeof(*available));
  if (!available) return 2;
  VkResult result = enumerate(pd, NULL, &count, available);
  unsigned advertised = 0;
  if (result == VK_SUCCESS) for (uint32_t i = 0; i < count; ++i)
    if (!strcmp(available[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) advertised = 1;
  free(available);
  CHECK(result);
  features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
  VkPhysicalDeviceFeatures2 out = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = features};
  query(pd, &out);
  printf("SCALED STRIDE extension=%u feature=%u\n", advertised, features->extendedDynamicState);
  if (!advertised || !features->extendedDynamicState) {
    puts("SCALED STRIDE UNSUPPORTED extended dynamic state");
    return 3;
  }
  extensions[0] = dc->ppEnabledExtensionNames[0];
  extensions[1] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
  dc->ppEnabledExtensionNames = extensions;
  dc->enabledExtensionCount = 2;
  features->pNext = (void *)dc->pNext;
  dc->pNext = features;
  return 0;
}

static void scaled_divisor_data(const struct scaled_case *format, unsigned divisor,
    uint32_t first, unsigned phase, unsigned char *data, float *expected) {
  float rows[8][4] = {{0}};
  unsigned stride = format->bits / 8 * format->components;
  int high = format->sign ? (1 << (format->bits - 1)) - 1 : (1 << format->bits) - 1;
  int low = format->sign ? -(1 << (format->bits - 1)) : 0;
  for (unsigned row = 0; row < 8; ++row) {
    rows[row][3] = 1;
    for (unsigned component = 0; component < format->components; ++component) {
      int delta = (int)(row * 17 + component * 3);
      int value = phase ? high - delta : low + delta;
      rows[row][component] = value;
      unsigned offset = row * stride + component * format->bits / 8;
      if (format->bits == 8) data[offset] = (uint8_t)value;
      else { uint16_t v = value; memcpy(data + offset, &v, 2); }
    }
  }
  for (unsigned instance = 0; instance < 4; ++instance) {
    unsigned row = first + (divisor ? instance / divisor : 0);
    memcpy(expected + instance * 4, rows[row], sizeof(rows[row]));
    if (phase == 2) expected[instance * 4] += 1;
  }
  memcpy(expected + 16, &first, sizeof(first));
}
#endif
