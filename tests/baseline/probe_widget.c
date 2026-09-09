#include "probe.h"

#include "widget_fixture.h"
#include "render_path.h"
#include "widget_pipeline.h"


static int ubo_draw_internal(int inject_wrong_binding, int validate, int dynamic, int large, int update_mode, int render_family, int entry_route) {
  struct render_path path = {.family = render_family, .route = entry_route};
  const int multi = dynamic == 2;
  const int staged = update_mode == 1;
  const int templated = update_mode == 2;
  const int recycle_descriptors = update_mode == 3 || update_mode == 4;
  const int repeat = staged || templated || recycle_descriptors;
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  if (templated || render_family) {
    PFN_vkEnumerateInstanceVersion version = (PFN_vkEnumerateInstanceVersion)gip(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t supported = VK_API_VERSION_1_0;
    if (!version) return 3;
    CHECK(version(&supported));
    uint32_t required = render_family ? render_path_api(render_family) : VK_API_VERSION_1_1;
    if (supported < required) { printf("UNSUPPORTED rendering instance API version\n"); return 3; }
  }
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-ubo",
                           .apiVersion = render_family ? render_path_api(render_family) :
                                         templated ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  const char *layer = "VK_LAYER_KHRONOS_validation";
  const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                              VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
  VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  struct validation_state validation = {0};
  VkDebugUtilsMessengerCreateInfoEXT debug = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = validation_message, .pUserData = &validation};
  VkValidationFeaturesEXT features = {
      .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
      .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
  if (validate) {
    ci.pNext = &features;
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = &layer;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
  }
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  if (validate) {
    V(vkCreateDebugUtilsMessengerEXT);
    destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (!destroy_messenger) return 2;
    CHECK(p_vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
  }
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceMemoryProperties);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  V(vkGetDeviceQueue);
  V(vkCreateBuffer);
  V(vkDestroyBuffer);
  V(vkGetBufferMemoryRequirements);
  V(vkAllocateMemory);
  V(vkFreeMemory);
  V(vkBindBufferMemory);
  V(vkMapMemory);
  V(vkUnmapMemory);
  V(vkCreateImage);
  V(vkDestroyImage);
  V(vkGetImageMemoryRequirements);
  V(vkBindImageMemory);
  V(vkCreateImageView);
  V(vkDestroyImageView);
  V(vkDestroyShaderModule);
  V(vkDestroyDescriptorSetLayout);
  V(vkDestroyPipelineLayout);
  V(vkDestroyRenderPass);
  V(vkDestroyFramebuffer);
  V(vkDestroyPipeline);
  V(vkCreateDescriptorPool);
  V(vkDestroyDescriptorPool);
  V(vkAllocateDescriptorSets);
  V(vkUpdateDescriptorSets);
  V(vkCreateCommandPool);
  V(vkDestroyCommandPool);
  V(vkAllocateCommandBuffers);
  V(vkBeginCommandBuffer);
  V(vkEndCommandBuffer);
  V(vkCmdBeginRenderPass);
  V(vkCmdEndRenderPass);
  V(vkCmdBindPipeline);
  V(vkCmdBindDescriptorSets);
  V(vkCmdBindIndexBuffer);
  V(vkCmdDrawIndexed);
  V(vkCmdCopyImageToBuffer);
  V(vkCmdPipelineBarrier);
  V(vkCreateFence);
  V(vkDestroyFence);
  V(vkQueueSubmit);
  V(vkWaitForFences);
  PFN_vkCmdCopyBuffer copy_buffer = NULL;
  PFN_vkResetCommandPool reset_pool = NULL;
  if (staged) {
    copy_buffer = (PFN_vkCmdCopyBuffer)gip(instance, "vkCmdCopyBuffer");
    if (!copy_buffer) return 2;
  }
  if (repeat) {
    reset_pool = (PFN_vkResetCommandPool)gip(instance, "vkResetCommandPool");
    if (!reset_pool) return 2;
  }
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
  if (templated) {
    V(vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties properties;
    p_vkGetPhysicalDeviceProperties(pd, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) {
      p_vkDestroyInstance(instance, NULL);
      return 3;
    }
  }
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, pd, &qi))
    return 2;
  uint32_t queue_count = 0;
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &queue_count, NULL);
  VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
  if (!queues) return 2;
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &queue_count, queues);
  for (qi = 0; qi < queue_count; ++qi)
    if (queues[qi].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
  free(queues);
  if (qi == queue_count) { printf("No graphics queue\n"); return 3; }
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  if (render_family) {
    int result = render_path_enable(&path, gip, instance, pd, &dc);
    if (result) {
      if (messenger) destroy_messenger(instance, messenger, NULL);
      p_vkDestroyInstance(instance, NULL);
      return result;
    }
  }
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  if (render_family) {
    int result = render_path_resolve(&path, h, gip, instance, device);
    if (result) {
      p_vkDestroyDevice(device, NULL);
      if (messenger) destroy_messenger(instance, messenger, NULL);
      p_vkDestroyInstance(instance, NULL);
      return result;
    }
  }
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkPhysicalDeviceMemoryProperties mp;
  p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);

  struct large_widget_ubo good, bad;
  uint32_t ubo_bytes = widget_fixture_data(large, dynamic, &good, &bad);
  const void *good_data = &good, *bad_data = &bad;

  VkDeviceSize stride = ubo_bytes;
  VkDeviceSize total = ubo_bytes;
  uint32_t dynamic_offset = 0;
  VkDescriptorType descriptor_type = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  if (dynamic) {
    V(vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties properties;
    p_vkGetPhysicalDeviceProperties(pd, &properties);
    VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
    if (!alignment || alignment > UINT32_MAX / 32) return 2;
    stride = ((ubo_bytes + alignment - 1) / alignment) * alignment;
    total = stride * (multi ? 11 : 3) + ubo_bytes;
    dynamic_offset = (uint32_t)(stride * (inject_wrong_binding ? 1 : 2));
    if (multi) printf("UBO_MULTI alignment=%llu stride=%llu range=%u total=%llu\n",
        (unsigned long long)alignment, (unsigned long long)stride, ubo_bytes, (unsigned long long)total);
    else printf("UBO_DYNAMIC alignment=%llu base=%llu dynamic=%u range=%u total=%llu\n",
        (unsigned long long)alignment, (unsigned long long)stride, dynamic_offset,
        ubo_bytes, (unsigned long long)total);
  }

  VkBufferCreateInfo ubo_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = total,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                   (staged ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0)};
  VkBuffer ubo_good, ubo_bad;
  CHECK(p_vkCreateBuffer(device, &ubo_ci, NULL, &ubo_good));
  CHECK(p_vkCreateBuffer(device, &ubo_ci, NULL, &ubo_bad));
  VkMemoryRequirements ubo_mr;
  p_vkGetBufferMemoryRequirements(device, ubo_good, &ubo_mr);
  int umi = find_mem(&mp, ubo_mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (umi < 0)
    return 3;
  VkMemoryAllocateInfo uma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = ubo_mr.size,
                              .memoryTypeIndex = (uint32_t)umi};
  VkDeviceMemory umem_good, umem_bad;
  CHECK(p_vkAllocateMemory(device, &uma, NULL, &umem_good));
  CHECK(p_vkAllocateMemory(device, &uma, NULL, &umem_bad));
  CHECK(p_vkBindBufferMemory(device, ubo_good, umem_good, 0));
  CHECK(p_vkBindBufferMemory(device, ubo_bad, umem_bad, 0));
  void *mapped;
  CHECK(p_vkMapMemory(device, umem_good, 0, total, 0, &mapped));
  if (multi) {
    widget_multi_data(mapped, stride, &good.widget, &bad.widget);
  } else if (dynamic) {
    memset(mapped, 0, (size_t)total);
    for (unsigned slot = 0; slot < 4; ++slot)
      memcpy((char *)mapped + stride * slot, slot == 3 ? good_data : bad_data, ubo_bytes);
  } else memcpy(mapped, good_data, ubo_bytes);
  p_vkUnmapMemory(device, umem_good);
  CHECK(p_vkMapMemory(device, umem_bad, 0, total, 0, &mapped));
  memcpy(mapped, bad_data, ubo_bytes);
  p_vkUnmapMemory(device, umem_bad);

  VkBuffer uploaded_ubo = VK_NULL_HANDLE;
  VkDeviceMemory uploaded_memory = VK_NULL_HANDLE;
  if (staged) {
    VkBufferCreateInfo upload_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ubo_bytes,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(p_vkCreateBuffer(device, &upload_ci, NULL, &uploaded_ubo));
    VkMemoryRequirements upload_mr;
    p_vkGetBufferMemoryRequirements(device, uploaded_ubo, &upload_mr);
    int upload_type = find_mem(&mp, upload_mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (upload_type < 0) return 3;
    VkMemoryAllocateInfo upload_ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = upload_mr.size, .memoryTypeIndex = (uint32_t)upload_type};
    CHECK(p_vkAllocateMemory(device, &upload_ai, NULL, &uploaded_memory));
    CHECK(p_vkBindBufferMemory(device, uploaded_ubo, uploaded_memory, 0));
  }

  uint16_t indices[kWidgetIndexCount] = {0, 1, 2, 0, 2, 3, 4, 5, 6,
                                         4, 6, 7, 8, 9, 10, 8, 10, 11};
  VkBufferCreateInfo ib_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = sizeof(indices),
                              .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
  VkBuffer ibo;
  CHECK(p_vkCreateBuffer(device, &ib_ci, NULL, &ibo));
  VkMemoryRequirements ib_mr;
  p_vkGetBufferMemoryRequirements(device, ibo, &ib_mr);
  int imi = find_mem(&mp, ib_mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (imi < 0) { printf("No coherent index memory\n"); return 3; }
  VkMemoryAllocateInfo ima = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = ib_mr.size,
                              .memoryTypeIndex = (uint32_t)imi};
  VkDeviceMemory imem;
  CHECK(p_vkAllocateMemory(device, &ima, NULL, &imem));
  CHECK(p_vkBindBufferMemory(device, ibo, imem, 0));
  CHECK(p_vkMapMemory(device, imem, 0, sizeof(indices), 0, &mapped));
  memcpy(mapped, indices, sizeof(indices));
  p_vkUnmapMemory(device, imem);

  VkImageCreateInfo img_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {kWidgetImage, kWidgetImage, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VkImage image;
  CHECK(p_vkCreateImage(device, &img_ci, NULL, &image));
  VkMemoryRequirements img_mr;
  p_vkGetImageMemoryRequirements(device, image, &img_mr);
  int img_mi = find_mem(&mp, img_mr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (img_mi < 0)
    img_mi = find_mem(&mp, img_mr.memoryTypeBits, 0);
  if (img_mi < 0) { printf("No compatible image memory\n"); return 3; }
  VkMemoryAllocateInfo img_ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = img_mr.size,
                                 .memoryTypeIndex = (uint32_t)img_mi};
  VkDeviceMemory img_mem;
  CHECK(p_vkAllocateMemory(device, &img_ma, NULL, &img_mem));
  CHECK(p_vkBindImageMemory(device, image, img_mem, 0));
  VkImageViewCreateInfo view_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  VkImageView view;
  CHECK(p_vkCreateImageView(device, &view_ci, NULL, &view));

  VkBufferCreateInfo rb_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = kWidgetImage * kWidgetImage * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer readback;
  CHECK(p_vkCreateBuffer(device, &rb_ci, NULL, &readback));
  VkMemoryRequirements rb_mr;
  p_vkGetBufferMemoryRequirements(device, readback, &rb_mr);
  int rmi = find_mem(&mp, rb_mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (rmi < 0) { printf("No coherent readback memory\n"); return 3; }
  VkMemoryAllocateInfo rma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = rb_mr.size,
                              .memoryTypeIndex = (uint32_t)rmi};
  VkDeviceMemory rmem;
  CHECK(p_vkAllocateMemory(device, &rma, NULL, &rmem));
  CHECK(p_vkBindBufferMemory(device, readback, rmem, 0));

  struct widget_pipeline prepared;
  int pipeline_result = widget_pipeline_create(gip, instance, device, view,
      large, multi, descriptor_type, render_family, &prepared);
  if (pipeline_result) return pipeline_result;
  VkShaderModule vs = prepared.vs, fs = prepared.fs;
  VkDescriptorSetLayout *set_layouts = prepared.set_layouts;
  VkDescriptorSetLayout set_layout = set_layouts[0];
  VkPipelineLayout pipeline_layout = prepared.layout;
  VkPipeline pipeline = prepared.pipeline;
  VkRenderPass rp = prepared.render_pass;
  VkFramebuffer fb = prepared.framebuffer;
  VkDescriptorPoolSize pool_size = {descriptor_type, multi ? 4 : 1};
  VkDescriptorPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = recycle_descriptors ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT : 0,
      .maxSets = multi ? 2 : 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size};
  VkDescriptorPool pool;
  CHECK(p_vkCreateDescriptorPool(device, &pool_ci, NULL, &pool));
  PFN_vkResetDescriptorPool reset_descriptors = NULL;
  PFN_vkFreeDescriptorSets free_descriptors = NULL;
  if (recycle_descriptors) {
    reset_descriptors = (PFN_vkResetDescriptorPool)gip(instance, "vkResetDescriptorPool");
    free_descriptors = (PFN_vkFreeDescriptorSets)gip(instance, "vkFreeDescriptorSets");
    if (!reset_descriptors || !free_descriptors) return 2;
    if (update_mode == 4) CHECK(reset_descriptors(device, pool, 0));
  }
  VkDescriptorSetAllocateInfo sa = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = multi ? 2 : 1,
      .pSetLayouts = set_layouts};
  VkDescriptorSet sets[2];
  CHECK(p_vkAllocateDescriptorSets(device, &sa, sets));
  VkDescriptorSet set = sets[0];
  VkDescriptorBufferInfo dbi = {.buffer = !dynamic && inject_wrong_binding ? ubo_bad
                                                               : ubo_good,
                                .offset = dynamic ? stride : 0,
                                .range = ubo_bytes};
  if (staged) dbi.buffer = uploaded_ubo;
  VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = 0,
                                .descriptorCount = 1,
                                .descriptorType = descriptor_type,
                                .pBufferInfo = &dbi};
  VkDescriptorUpdateTemplate update_template = VK_NULL_HANDLE;
  PFN_vkUpdateDescriptorSetWithTemplate update_with_template = NULL;
  PFN_vkDestroyDescriptorUpdateTemplate destroy_template = NULL;
  if (templated) {
    V(vkCreateDescriptorUpdateTemplate);
    update_with_template = (PFN_vkUpdateDescriptorSetWithTemplate)gip(instance, "vkUpdateDescriptorSetWithTemplate");
    destroy_template = (PFN_vkDestroyDescriptorUpdateTemplate)gip(instance, "vkDestroyDescriptorUpdateTemplate");
    if (!update_with_template || !destroy_template) return 2;
    VkDescriptorUpdateTemplateEntry entry = {.dstBinding = 0, .descriptorCount = 1,
        .descriptorType = descriptor_type, .offset = sizeof(VkDescriptorBufferInfo),
        .stride = sizeof(VkDescriptorBufferInfo)};
    VkDescriptorUpdateTemplateCreateInfo template_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 1, .pDescriptorUpdateEntries = &entry,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = set_layout};
    CHECK(p_vkCreateDescriptorUpdateTemplate(device, &template_ci, NULL, &update_template));
  } else if (multi) {
    VkDescriptorBufferInfo infos[4];
    const unsigned bases[4] = {1, 2, 1, 3};
    for (unsigned i = 0; i < 4; ++i)
      infos[i] = (VkDescriptorBufferInfo){ubo_good, stride * bases[i], ubo_bytes};
    VkWriteDescriptorSet writes[3] = {write, write, write};
    writes[0].dstBinding = 3; writes[0].descriptorCount = 2; writes[0].pBufferInfo = &infos[1];
    writes[1].pBufferInfo = &infos[0];
    writes[2].dstSet = sets[1]; writes[2].dstBinding = 1; writes[2].pBufferInfo = &infos[3];
    p_vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
  } else p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
  printf("DRAW set=%p layout=%p pipeline=%p buffer=%p range=%u inject=%d\n",
         (void *)set, (void *)pipeline_layout, (void *)pipeline,
         (void *)dbi.buffer, ubo_bytes, inject_wrong_binding);

  VkCommandPoolCreateInfo cpc = {.sType =
                                     VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .queueFamilyIndex = qi};
  VkCommandPool cpool;
  CHECK(p_vkCreateCommandPool(device, &cpc, NULL, &cpool));
  VkCommandBufferAllocateInfo cba_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cpool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  VkCommandBuffer cb;
  CHECK(p_vkAllocateCommandBuffers(device, &cba_info, &cb));
  int match_good = 0, match_bad = 0, dump_failed = 0, pixels_failed = 0;
  for (unsigned submission = 0; submission < (recycle_descriptors ? 8u : repeat ? 6u : 1u); ++submission) {
    if (repeat) {
      inject_wrong_binding = recycle_descriptors ? (submission / 2) % 2 : submission / 2 == 1;
      printf("UBO_%s bytes=%u submission=%u rerecord=%u alternate=%d\n",
             recycle_descriptors ? "POOL_RECYCLE" : staged ? "STAGED" : "TEMPLATE", ubo_bytes, submission,
             submission % 2 == 0, inject_wrong_binding);
    }
    if (!repeat || submission % 2 == 0) {
      if (submission) CHECK(reset_pool(device, cpool, 0));
      if (recycle_descriptors && submission) {
        /* All earlier uses completed at the previous fence. Exercise reset
         * twice, then individual free/reallocation, with fresh descriptors
         * and alternating shader-visible values. Odd submissions reuse the
         * same recording without changing its descriptor set. */
        if (submission == 6) CHECK(free_descriptors(device, pool, 1, sets));
        else CHECK(reset_descriptors(device, pool, 0));
        CHECK(p_vkAllocateDescriptorSets(device, &sa, sets));
        set = sets[0];
        dbi.buffer = inject_wrong_binding ? ubo_bad : ubo_good;
        write.dstSet = set;
        p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
      }
      if (templated) {
        /* The prefix is a valid opposite descriptor. Ignoring entry.offset
         * therefore produces a definite incorrect pixel instead of a fault. */
        VkDescriptorBufferInfo payload[2] = {
            {.buffer = inject_wrong_binding ? ubo_good : ubo_bad, .range = ubo_bytes},
            {.buffer = inject_wrong_binding ? ubo_bad : ubo_good, .range = ubo_bytes}};
        update_with_template(device, set, update_template, payload);
        printf("UBO_TEMPLATE_UPDATE offset=%zu buffer=%p range=%u\n",
               sizeof(payload[0]), (void *)payload[1].buffer, ubo_bytes);
      }
      VkCommandBufferBeginInfo begin = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(p_vkBeginCommandBuffer(cb, &begin));
      if (repeat) {
        /* Cover the previous submission's UBO reads, attachment/readback use,
         * then make this copy visible to both shader stages. */
        VkMemoryBarrier reuse = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        p_vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 1, &reuse, 0, NULL, 0, NULL);
      }
      if (staged) {
        VkBufferCopy upload = {.size = ubo_bytes};
        copy_buffer(cb, inject_wrong_binding ? ubo_bad : ubo_good, uploaded_ubo, 1, &upload);
        VkMemoryBarrier ready = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT};
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &ready, 0, NULL, 0, NULL);
      }
      VkClearValue clear = {.color = {{0, 0, 0, 0}}};
      VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                    .renderPass = rp,
                                    .framebuffer = fb,
                                    .renderArea = {{0, 0}, {kWidgetImage, kWidgetImage}},
                                    .clearValueCount = 1,
                                    .pClearValues = &clear};
      if (render_family) render_path_begin(&path, cb, image, view, kWidgetImage, kWidgetImage);
      else p_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      uint32_t multi_offsets[4] = {(uint32_t)(stride * 2), (uint32_t)(stride * 3),
          (uint32_t)(stride * (inject_wrong_binding ? 5 : 6)), (uint32_t)(stride * 8)};
      p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, multi ? 2 : 1, sets, multi ? 4 : dynamic ? 1 : 0,
                                multi ? multi_offsets : dynamic ? &dynamic_offset : NULL);
      if (multi) printf("UBO_MULTI stride=%llu offsets=%u,%u,%u,%u alternate_set=0 binding=3 element=1\n",
          (unsigned long long)stride, multi_offsets[0], multi_offsets[1], multi_offsets[2], multi_offsets[3]);
      p_vkCmdBindIndexBuffer(cb, ibo, 0, VK_INDEX_TYPE_UINT16);
      p_vkCmdDrawIndexed(cb, kWidgetIndexCount, 1, 0, 0, 0);
      if (render_family) render_path_end(&path, cb, image);
      else p_vkCmdEndRenderPass(cb);
      VkBufferImageCopy copy = {
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .imageExtent = {kWidgetImage, kWidgetImage, 1}};
      p_vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &copy);
      VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      if (render_family) render_path_to_host(&path, cb);
      else p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
      CHECK(p_vkEndCommandBuffer(cb));
    }
    VkFence fence;
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cb};
    if (render_family) CHECK(render_path_submit(&path, queue, cb, fence));
    else CHECK(p_vkQueueSubmit(queue, 1, &si, fence));
    CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
    uint8_t *pixels = NULL;
    CHECK(p_vkMapMemory(device, rmem, 0, kWidgetImage * kWidgetImage * 4, 0,
                        (void **)&pixels));
    uint8_t *mid = pixels + (8 * kWidgetImage + 8) * 4;
    printf("PIXEL mid rgba=%u,%u,%u,%u inject=%d\n", mid[0], mid[1], mid[2],
           mid[3], inject_wrong_binding);
    match_good = mid[0] == 255 && mid[1] == 255 && mid[2] == 0 && mid[3] == 255;
    match_bad = mid[0] == 0 && mid[1] == 255 && mid[2] == 255 && mid[3] == 0;
    if (multi || recycle_descriptors) {
      const uint8_t expected[2][4] = {{255, 255, 0, 255}, {0, 255, 255, 0}};
      unsigned mismatches = 0;
      for (unsigned pixel = 0; pixel < kWidgetImage * kWidgetImage; ++pixel)
        mismatches += memcmp(pixels + 4 * pixel, expected[inject_wrong_binding], 4) != 0;
      printf("UBO_%s pixels=%u mismatches=%u\n",
             recycle_descriptors ? "POOL_RECYCLE" : "MULTI", kWidgetImage * kWidgetImage, mismatches);
      pixels_failed |= mismatches != 0;
    }
    const char *dump_dir = getenv("PROBE_WIDGET_DUMP_DIR");
    if (dump_dir) {
      char path[4096];
      int length = snprintf(path, sizeof(path), "%s/widget-%s.rgba", dump_dir,
                            inject_wrong_binding ? "bad" : "good");
      FILE *file = length >= 0 && (size_t)length < sizeof(path) ? fopen(path, "wb") : NULL;
      if (!file) {
        fprintf(stderr, "Cannot open widget pixel dump\n");
        dump_failed = 1;
      } else {
        dump_failed |= fwrite(pixels, 1, kWidgetImage * kWidgetImage * 4, file) !=
                      kWidgetImage * kWidgetImage * 4;
        if (fclose(file)) dump_failed = 1;
      }
    }
    p_vkUnmapMemory(device, rmem);
    p_vkDestroyFence(device, fence, NULL);
    if (inject_wrong_binding ? !match_bad : !match_good) {
      printf("UBO pixel mismatch submission=%u alternate=%d\n", submission, inject_wrong_binding);
      pixels_failed = 1;
    }
  }
  p_vkDestroyCommandPool(device, cpool, NULL);
  p_vkDestroyPipeline(device, pipeline, NULL);
  if (fb) p_vkDestroyFramebuffer(device, fb, NULL);
  if (rp) p_vkDestroyRenderPass(device, rp, NULL);
  p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
  if (templated) destroy_template(device, update_template, NULL);
  p_vkDestroyDescriptorPool(device, pool, NULL);
  p_vkDestroyDescriptorSetLayout(device, set_layout, NULL);
  if (multi) p_vkDestroyDescriptorSetLayout(device, set_layouts[1], NULL);
  p_vkDestroyShaderModule(device, vs, NULL);
  p_vkDestroyShaderModule(device, fs, NULL);
  p_vkDestroyImageView(device, view, NULL);
  p_vkDestroyImage(device, image, NULL);
  p_vkDestroyBuffer(device, readback, NULL);
  p_vkDestroyBuffer(device, ibo, NULL);
  if (staged) {
    p_vkDestroyBuffer(device, uploaded_ubo, NULL);
    p_vkFreeMemory(device, uploaded_memory, NULL);
  }
  p_vkDestroyBuffer(device, ubo_good, NULL);
  p_vkDestroyBuffer(device, ubo_bad, NULL);
  p_vkFreeMemory(device, img_mem, NULL);
  p_vkFreeMemory(device, rmem, NULL);
  p_vkFreeMemory(device, imem, NULL);
  p_vkFreeMemory(device, umem_good, NULL);
  p_vkFreeMemory(device, umem_bad, NULL);
  p_vkDestroyDevice(device, NULL);
  if (validate) {
    probe_mappings("widget-validation-active");
    destroy_messenger(instance, messenger, NULL);
  }
  p_vkDestroyInstance(instance, NULL);
  if (validate) {
    printf("WIDGET validation errors=%u binding=%d\n", validation.errors, inject_wrong_binding);
    if (validation.errors) return 2;
  }
  if (dump_failed || pixels_failed) return 2;
  if (inject_wrong_binding) {
    if (!match_bad) {
      printf("UBO negative-control FAIL (unexpected pixel)\n");
      return 2;
    }
    printf("UBO negative-control PASS (known alternate descriptor observed)\n");
    return 0;
  }
  if (!match_good) {
    printf("UBO shader did not read parameters/mvp/srgb\n");
    return 2;
  }
  printf("UBO shader-read PASS\n");
  return 0;
}

int ubo_probe(void) {
  int good = ubo_draw(0, 0);
  if (good)
    return good;
  int bad = ubo_draw(1, 0);
  if (bad)
    return bad;
  printf("UBO PASS\n");
  return 0;
}

int ubo_validation_probe(void) {
  int result = ubo_draw(0, 1);
  if (result) return result;
  return ubo_draw(1, 1);
}


int ubo_draw(int inject_wrong_binding, int validate) {
  return ubo_draw_internal(inject_wrong_binding, validate, 0, 0, 0, 0, 0);
}

int ubo_dynamic_probe(int validate) {
  int result = ubo_draw_internal(0, validate, 1, 0, 0, 0, 0);
  if (result) return result;
  return ubo_draw_internal(1, validate, 1, 0, 0, 0, 0);
}

int ubo_large_probe(int validate) {
  for (int dynamic = 0; dynamic < 2; ++dynamic)
    for (int alternate = 0; alternate < 2; ++alternate) {
      int result = ubo_draw_internal(alternate, validate, dynamic, 1, 0, 0, 0);
      if (result) return result;
    }
  return 0;
}

int ubo_staged_probe(int validate) {
  for (int large = 0; large < 2; ++large) {
    int result = ubo_draw_internal(0, validate, 0, large, 1, 0, 0);
    if (result) return result;
  }
  return 0;
}

int ubo_template_probe(int validate) {
  for (int large = 0; large < 2; ++large) {
    int result = ubo_draw_internal(0, validate, 0, large, 2, 0, 0);
    if (result) return result;
  }
  return 0;
}

int ubo_pool_reset_probe(int validate, int initially_empty) {
  return ubo_draw_internal(0, validate, 0, 0, initially_empty ? 4 : 3, 0, 0);
}

int ubo_dynamic_draw(int alternate) {
  return ubo_draw_internal(alternate, 0, 1, 0, 0, 0, 0);
}

int ubo_render_probe(int family, int route, int validate) {
  int first = route < 0 ? 0 : route, last = route < 0 ? 1 : route;
  for (int entry = first; entry <= last; ++entry)
    for (int alternate = 0; alternate < 2; ++alternate) {
      int result = ubo_draw_internal(alternate, validate, 0, 0, 0, family, entry);
      if (result) return result;
    }
  return 0;
}

int ubo_multi_draw(int alternate, int validate) {
  return ubo_draw_internal(alternate, validate, 2, 0, 0, 0, 0);
}
int ubo_multi_probe(int validate) {
  int result = ubo_multi_draw(0, validate);
  return result ? result : ubo_multi_draw(1, validate);
}
