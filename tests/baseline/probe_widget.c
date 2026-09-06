#include "probe.h"

#include "shaders/widget.vert.inc"
#include "shaders/widget.frag.inc"


enum {
  kWidgetUboBytes = 272,
  kWidgetIndexCount = 18,
  kWidgetImage = 16
};

struct widget_ubo {
  float parameters[12][4];
  float mvp[16];
  float checker[3];
  int srgbTarget;
};

static int ubo_draw_internal(int inject_wrong_binding, int validate, int dynamic) {
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
                           .pApplicationName = "hybris-ubo",
                           .apiVersion = VK_API_VERSION_1_0};
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
  V(vkCreateShaderModule);
  V(vkDestroyShaderModule);
  V(vkCreateDescriptorSetLayout);
  V(vkDestroyDescriptorSetLayout);
  V(vkCreatePipelineLayout);
  V(vkDestroyPipelineLayout);
  V(vkCreateRenderPass);
  V(vkDestroyRenderPass);
  V(vkCreateFramebuffer);
  V(vkDestroyFramebuffer);
  V(vkCreateGraphicsPipelines);
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
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
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
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkPhysicalDeviceMemoryProperties mp;
  p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);

  struct widget_ubo good = {0};
  struct widget_ubo bad = {0};
  good.parameters[0][0] = 1.0f;
  good.mvp[0] = 1.0f;
  good.mvp[5] = 1.0f;
  good.mvp[10] = 1.0f;
  good.mvp[15] = 1.0f;
  good.checker[0] = 0.0f;
  good.srgbTarget = 1;
  /* Keep identity MVP so the triangle still covers the readback pixel.
   * Only fragment-encoded fields differ. */
  bad.parameters[0][0] = 0.0f;
  bad.mvp[0] = 1.0f;
  bad.mvp[5] = 1.0f;
  bad.mvp[10] = 1.0f;
  bad.mvp[15] = 1.0f;
  bad.checker[0] = 1.0f;
  bad.srgbTarget = 0;
  if (sizeof(good) != kWidgetUboBytes) {
    printf("UBO sizeof=%zu expected=%d\n", sizeof(good), kWidgetUboBytes);
    return 2;
  }
  printf("UBO layout parameters@0 mvp@192 checker@256 srgb@268 size=%zu\n",
         sizeof(good));

  VkDeviceSize stride = kWidgetUboBytes;
  VkDeviceSize total = kWidgetUboBytes;
  uint32_t dynamic_offset = 0;
  VkDescriptorType descriptor_type = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  if (dynamic) {
    V(vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties properties;
    p_vkGetPhysicalDeviceProperties(pd, &properties);
    VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment;
    if (!alignment || alignment > UINT32_MAX / 8) return 2;
    stride = ((kWidgetUboBytes + alignment - 1) / alignment) * alignment;
    total = stride * 3 + kWidgetUboBytes;
    dynamic_offset = (uint32_t)(stride * (inject_wrong_binding ? 1 : 2));
    printf("UBO_DYNAMIC alignment=%llu base=%llu dynamic=%u range=%u total=%llu\n",
           (unsigned long long)alignment, (unsigned long long)stride, dynamic_offset,
           kWidgetUboBytes, (unsigned long long)total);
  }

  VkBufferCreateInfo ubo_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = total,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
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
  if (dynamic) {
    memset(mapped, 0, (size_t)total);
    for (unsigned slot = 0; slot < 4; ++slot)
      memcpy((char *)mapped + stride * slot, slot == 3 ? &good : &bad, sizeof(good));
  } else memcpy(mapped, &good, sizeof(good));
  p_vkUnmapMemory(device, umem_good);
  CHECK(p_vkMapMemory(device, umem_bad, 0, total, 0, &mapped));
  memcpy(mapped, &bad, sizeof(bad));
  p_vkUnmapMemory(device, umem_bad);

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

  VkShaderModuleCreateInfo vs_ci = {.sType =
                                        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = kWidgetVertSpv_word_count * 4,
                                    .pCode = kWidgetVertSpv};
  VkShaderModuleCreateInfo fs_ci = {.sType =
                                        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = kWidgetFragSpv_word_count * 4,
                                    .pCode = kWidgetFragSpv};
  VkShaderModule vs, fs;
  CHECK(p_vkCreateShaderModule(device, &vs_ci, NULL, &vs));
  CHECK(p_vkCreateShaderModule(device, &fs_ci, NULL, &fs));
  VkDescriptorSetLayoutBinding bind = {
      .binding = 0,
      .descriptorType = descriptor_type,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo sl_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &bind};
  VkDescriptorSetLayout set_layout;
  CHECK(p_vkCreateDescriptorSetLayout(device, &sl_ci, NULL, &set_layout));
  VkPipelineLayoutCreateInfo pl_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout};
  VkPipelineLayout pipeline_layout;
  CHECK(p_vkCreatePipelineLayout(device, &pl_ci, NULL, &pipeline_layout));
  VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout =
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
  VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color_ref};
  VkSubpassDependency to_copy = {
      .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
  VkRenderPassCreateInfo rp_ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .dependencyCount = 1,
                                  .pDependencies = &to_copy,
                                  .attachmentCount = 1,
                                  .pAttachments = &att,
                                  .subpassCount = 1,
                                  .pSubpasses = &sub};
  VkRenderPass rp;
  CHECK(p_vkCreateRenderPass(device, &rp_ci, NULL, &rp));
  VkFramebufferCreateInfo fb_ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = rp,
                                   .attachmentCount = 1,
                                   .pAttachments = &view,
                                   .width = kWidgetImage,
                                   .height = kWidgetImage,
                                   .layers = 1};
  VkFramebuffer fb;
  CHECK(p_vkCreateFramebuffer(device, &fb_ci, NULL, &fb));
  VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs,
       .pName = "main"}};
  VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport vp = {0, 0, (float)kWidgetImage, (float)kWidgetImage, 0, 1};
  VkRect2D sc = {{0, 0}, {kWidgetImage, kWidgetImage}};
  VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &vp,
      .scissorCount = 1,
      .pScissors = &sc};
  VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
  VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba};
  VkGraphicsPipelineCreateInfo gp = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vps,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &blend,
      .layout = pipeline_layout,
      .renderPass = rp};
  VkPipeline pipeline;
  CHECK(p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL,
                                    &pipeline));
  VkDescriptorPoolSize pool_size = {descriptor_type, 1};
  VkDescriptorPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size};
  VkDescriptorPool pool;
  CHECK(p_vkCreateDescriptorPool(device, &pool_ci, NULL, &pool));
  VkDescriptorSetAllocateInfo sa = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout};
  VkDescriptorSet set;
  CHECK(p_vkAllocateDescriptorSets(device, &sa, &set));
  VkDescriptorBufferInfo dbi = {.buffer = !dynamic && inject_wrong_binding ? ubo_bad
                                                               : ubo_good,
                                .offset = dynamic ? stride : 0,
                                .range = kWidgetUboBytes};
  VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = 0,
                                .descriptorCount = 1,
                                .descriptorType = descriptor_type,
                                .pBufferInfo = &dbi};
  p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
  printf("DRAW set=%p layout=%p pipeline=%p buffer=%p range=%u inject=%d\n",
         (void *)set, (void *)pipeline_layout, (void *)pipeline,
         (void *)dbi.buffer, kWidgetUboBytes, inject_wrong_binding);

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
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(p_vkBeginCommandBuffer(cb, &begin));
  VkClearValue clear = {.color = {{0, 0, 0, 0}}};
  VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = rp,
                                .framebuffer = fb,
                                .renderArea = {{0, 0}, {kWidgetImage, kWidgetImage}},
                                .clearValueCount = 1,
                                .pClearValues = &clear};
  p_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                            0, 1, &set, dynamic ? 1 : 0, dynamic ? &dynamic_offset : NULL);
  p_vkCmdBindIndexBuffer(cb, ibo, 0, VK_INDEX_TYPE_UINT16);
  p_vkCmdDrawIndexed(cb, kWidgetIndexCount, 1, 0, 0, 0);
  p_vkCmdEndRenderPass(cb);
  VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {kWidgetImage, kWidgetImage, 1}};
  p_vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &copy);
  VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
  CHECK(p_vkEndCommandBuffer(cb));
  VkFence fence;
  VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                     .commandBufferCount = 1,
                     .pCommandBuffers = &cb};
  CHECK(p_vkQueueSubmit(queue, 1, &si, fence));
  CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
  uint8_t *pixels = NULL;
  CHECK(p_vkMapMemory(device, rmem, 0, kWidgetImage * kWidgetImage * 4, 0,
                      (void **)&pixels));
  uint8_t *mid = pixels + (8 * kWidgetImage + 8) * 4;
  printf("PIXEL mid rgba=%u,%u,%u,%u inject=%d\n", mid[0], mid[1], mid[2],
         mid[3], inject_wrong_binding);
  int match_good = mid[0] == 255 && mid[1] == 255 && mid[2] == 0 && mid[3] == 255;
  int match_bad = mid[0] == 0 && mid[1] == 255 && mid[2] == 255 && mid[3] == 0;
  const char *dump_dir = getenv("PROBE_WIDGET_DUMP_DIR");
  int dump_failed = 0;
  if (dump_dir) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/widget-%s.rgba", dump_dir,
                          inject_wrong_binding ? "bad" : "good");
    FILE *file = length >= 0 && (size_t)length < sizeof(path) ? fopen(path, "wb") : NULL;
    if (!file) {
      fprintf(stderr, "Cannot open widget pixel dump\n");
      dump_failed = 1;
    } else {
      dump_failed = fwrite(pixels, 1, kWidgetImage * kWidgetImage * 4, file) !=
                    kWidgetImage * kWidgetImage * 4;
      if (fclose(file)) dump_failed = 1;
    }
  }
  p_vkUnmapMemory(device, rmem);
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroyCommandPool(device, cpool, NULL);
  p_vkDestroyPipeline(device, pipeline, NULL);
  p_vkDestroyFramebuffer(device, fb, NULL);
  p_vkDestroyRenderPass(device, rp, NULL);
  p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
  p_vkDestroyDescriptorPool(device, pool, NULL);
  p_vkDestroyDescriptorSetLayout(device, set_layout, NULL);
  p_vkDestroyShaderModule(device, vs, NULL);
  p_vkDestroyShaderModule(device, fs, NULL);
  p_vkDestroyImageView(device, view, NULL);
  p_vkDestroyImage(device, image, NULL);
  p_vkDestroyBuffer(device, readback, NULL);
  p_vkDestroyBuffer(device, ibo, NULL);
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
  if (dump_failed) return 2;
  if (validate) {
    printf("WIDGET validation errors=%u binding=%d\n", validation.errors, inject_wrong_binding);
    if (validation.errors) return 2;
  }
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
  return ubo_draw_internal(inject_wrong_binding, validate, 0);
}

int ubo_dynamic_probe(int validate) {
  int result = ubo_draw_internal(0, validate, 1);
  if (result) return result;
  return ubo_draw_internal(1, validate, 1);
}
