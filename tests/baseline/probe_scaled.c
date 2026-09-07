/* SPDX-License-Identifier: Apache-2.0 */
#include "probe.h"
#include "allocation_fixture.h"
#include "shaders/scaled.vert.inc"
#include "shaders/scaled.multi.inc"
#include "shaders/scaled.literal.inc"
#include "shaders/scaled.frag.inc"
enum { kScaledImage = 16 };
static const struct scaled_case { VkFormat format; const char *name; unsigned bits, components, sign; } cases[] = {
#define CASE(n,b,c) {VK_FORMAT_##n##_USCALED, #n "_USCALED", b,c,0}, {VK_FORMAT_##n##_SSCALED, #n "_SSCALED",b,c,1}
 CASE(R8,8,1), CASE(R8G8,8,2), CASE(R8G8B8A8,8,4),
 CASE(R16,16,1), CASE(R16G16,16,2), CASE(R16G16B16A16,16,4)
#undef CASE
};
int scaled_vertex_probe(int validate, int route, int variant) {
  const int multiple = variant == 1, literal = variant == 2;
  struct allocation_probe allocations = {0};
  VkAllocationCallbacks callbacks = {.pUserData = &allocations,
    .pfnAllocation = instance_allocate, .pfnReallocation = instance_reallocate, .pfnFree = instance_free};
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
    .pApplicationName = "hybris-scaled", .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
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
  V(vkCreatePipelineLayout);
  V(vkDestroyPipelineLayout);
  V(vkCreateRenderPass);
  V(vkDestroyRenderPass);
  V(vkCreateFramebuffer);
  V(vkDestroyFramebuffer);
  V(vkCreateGraphicsPipelines);
  V(vkDestroyPipeline);
  V(vkCreateCommandPool);
  V(vkDestroyCommandPool);
  V(vkAllocateCommandBuffers);
  V(vkBeginCommandBuffer);
  V(vkEndCommandBuffer);
  V(vkCmdBeginRenderPass);
  V(vkCmdEndRenderPass);
  V(vkCmdBindPipeline);
  V(vkCmdCopyImageToBuffer);
  V(vkCmdPipelineBarrier);
  V(vkCreateFence);
  V(vkDestroyFence);
  V(vkQueueSubmit);
  V(vkWaitForFences);
  V(vkGetPhysicalDeviceFormatProperties);
  V(vkGetPhysicalDeviceFormatProperties2);
  V(vkCmdBindVertexBuffers);
  V(vkCmdDraw);
  V(vkCmdPushConstants);
  V(vkResetCommandPool);
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
  if (route) {
    V(vkGetDeviceProcAddr);
#ifdef HYBRIS_PROBE_LINKED
#define LINKED(n) n
#else
#define LINKED(n) NULL
#endif
#define DEVICE(n) do { \
    if (route == 1) p_##n = (PFN_##n)p_vkGetDeviceProcAddr(device, #n); \
    else if (route == 2) p_##n = (PFN_##n)sym(h, #n); \
    else p_##n = LINKED(n); \
    if (!p_##n) return 2; \
  } while (0)
    DEVICE(vkCreateShaderModule); DEVICE(vkDestroyShaderModule); DEVICE(vkCreateGraphicsPipelines);
#undef DEVICE
#undef LINKED
  }
  printf("SCALED route=%d validation=%d\n", route, validate);
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkPhysicalDeviceMemoryProperties mp;
  p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  void *mapped;
  uint16_t indices[12] = {0};
  VkBufferCreateInfo ib_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = sizeof(indices),
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
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
      .extent = {kScaledImage, kScaledImage, 1},
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
                              .size = kScaledImage * kScaledImage * 4,
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

  VkShaderModuleCreateInfo vs_ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = multiple ? sizeof(kScaledMultiSpv) : literal ? sizeof(kScaledLiteralSpv) : sizeof(kScaledVertSpv),
    .pCode = multiple ? kScaledMultiSpv : literal ? kScaledLiteralSpv : kScaledVertSpv};
  VkShaderModuleCreateInfo fs_ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = sizeof(kScaledFragSpv), .pCode = kScaledFragSpv};
  VkShaderModule vs, fs;
  CHECK(p_vkCreateShaderModule(device, &vs_ci, &callbacks, &vs));
  if (multiple) fs = vs;
  else CHECK(p_vkCreateShaderModule(device, &fs_ci, &callbacks, &fs));
  VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pl_ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
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
  VkRenderPass rp = VK_NULL_HANDLE;
  CHECK(p_vkCreateRenderPass(device, &rp_ci, NULL, &rp));
  VkFramebufferCreateInfo fb_ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = rp,
                                   .attachmentCount = 1,
                                   .pAttachments = &view,
                                   .width = kScaledImage,
                                   .height = kScaledImage,
                                   .layers = 1};
  VkFramebuffer fb = VK_NULL_HANDLE;
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
  VkViewport vp = {0, 0, (float)kScaledImage, (float)kScaledImage, 0, 1};
  VkRect2D sc = {{0, 0}, {kScaledImage, kScaledImage}};
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
  VkVertexInputBindingDescription binding = {.binding = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attribute = {.location = 0, .binding = 0};
  vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &attribute;
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
  unsigned tested = 0, unsupported = 0, failures = 0;
  for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); ++c) {
    const struct scaled_case *f = &cases[c];
    VkFormatProperties props;
    p_vkGetPhysicalDeviceFormatProperties(pd, f->format, &props);
    VkFormatProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    p_vkGetPhysicalDeviceFormatProperties2(pd, f->format, &props2);
    if (props.bufferFeatures != props2.formatProperties.bufferFeatures) {
      printf("SCALED query mismatch format=%s\n", f->name); return 2;
    }
    if (!(props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
      printf("SCALED UNSUPPORTED format=%s\n", f->name); ++unsupported; continue;
    }
    int alternate_entry = multiple && (c % 2);
    stages[0].pName = multiple ? (alternate_entry ? "alternate_vertex" : "scaled_vertex") : "main";
    attribute.format = f->format;
    binding.stride = f->bits / 8 * f->components;
    VkPipeline pipeline;
    CHECK(p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, &callbacks, &pipeline));
    for (unsigned phase = 0; phase < 3; ++phase) {
      float expected[4] = {0,0,0,1};
      unsigned char data[24] = {0};
      for (unsigned component = 0; component < f->components; ++component) {
        int high = f->sign ? (1 << (f->bits - 1)) - 1 : (1 << f->bits) - 1;
        int low = f->sign ? -(1 << (f->bits - 1)) : 0;
        int value = (component + phase) % 2 ? high : low;
        expected[component] = value;
        for (unsigned vertex = 0; vertex < 3; ++vertex) {
          unsigned offset = vertex * binding.stride + component * f->bits / 8;
          if (f->bits == 8) data[offset] = (uint8_t)value;
          else { uint16_t v = value; memcpy(data + offset, &v, 2); }
        }
      }
      if (phase == 2) expected[0] += 1;
      CHECK(p_vkMapMemory(device, imem, 0, sizeof(data), 0, &mapped));
      memcpy(mapped, data, sizeof(data));
      p_vkUnmapMemory(device, imem);
      CHECK(p_vkResetCommandPool(device, cpool, 0));
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(p_vkBeginCommandBuffer(cb, &begin));
      VkMemoryBarrier reuse = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
      p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &reuse, 0, NULL, 0, NULL);
      VkClearValue clear = {.color = {{0,0,0,0}}};
      VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb, .renderArea = {{0,0},{kScaledImage,kScaledImage}},
        .clearValueCount = 1, .pClearValues = &clear};
      p_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
      p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      VkDeviceSize offset = 0;
      p_vkCmdBindVertexBuffers(cb, 0, 1, &ibo, &offset);
      p_vkCmdPushConstants(cb, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, expected);
      p_vkCmdDraw(cb, 3, 1, 0, 0);
      p_vkCmdEndRenderPass(cb);
      VkBufferImageCopy copy = {
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .imageExtent = {kScaledImage, kScaledImage, 1}};
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
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(p_vkQueueSubmit(queue, 1, &si, fence));
      CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
      uint8_t *pixels;
      CHECK(p_vkMapMemory(device, rmem, 0, kScaledImage*kScaledImage*4, 0, (void **)&pixels));
      unsigned bad = 0;
      for (unsigned pixel = 0; pixel < kScaledImage*kScaledImage; ++pixel)
        for (unsigned component = 0; component < 4; ++component) {
          unsigned expected_byte = phase == 2 && component == 0 ? 0 : 255;
          if (alternate_entry) expected_byte = 255 - expected_byte;
          if (pixels[pixel*4+component] != expected_byte) ++bad;
        }
      printf("SCALED format=%s entry=%s phase=%u expected=%g,%g,%g,%g pixel=%u,%u,%u,%u bad=%u\n",
        f->name, stages[0].pName, phase, expected[0], expected[1], expected[2], expected[3], pixels[0],pixels[1],pixels[2],pixels[3],bad);
      failures += !!bad;
      p_vkUnmapMemory(device, rmem);
      p_vkDestroyFence(device, fence, NULL);
    }
    p_vkDestroyPipeline(device, pipeline, &callbacks);
    ++tested;
  }
  p_vkDestroyCommandPool(device, cpool, NULL);
  p_vkDestroyFramebuffer(device, fb, NULL);
  p_vkDestroyRenderPass(device, rp, NULL);
  p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
  p_vkDestroyShaderModule(device, vs, &callbacks);
  if (!multiple) p_vkDestroyShaderModule(device, fs, &callbacks);
  p_vkDestroyImageView(device, view, NULL);
  p_vkDestroyImage(device, image, NULL);
  p_vkDestroyBuffer(device, readback, NULL);
  p_vkDestroyBuffer(device, ibo, NULL);
  p_vkFreeMemory(device, img_mem, NULL);
  p_vkFreeMemory(device, rmem, NULL);
  p_vkFreeMemory(device, imem, NULL);
  p_vkDestroyDevice(device, NULL);
  if (messenger) destroy_messenger(instance, messenger, NULL);
  p_vkDestroyInstance(instance, NULL);
  dlclose(h);
  printf("SCALED allocator calls=%u live=%u\n", allocations.calls, allocations.live);
  failures += allocations.live != 0;
  printf("SCALED tested=%u unsupported=%u failures=%u validation_errors=%u\n", tested, unsupported, failures, validation.errors);
  return failures || validation.errors ? 2 : unsupported ? 3 : 0;
}
