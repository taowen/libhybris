#ifndef HYBRIS_WIDGET_PIPELINE_H
#define HYBRIS_WIDGET_PIPELINE_H

#include "shaders/widget.vert.inc"
#include "shaders/widget.frag.inc"
#include "shaders/widget-large.vert.inc"
#include "shaders/widget-large.frag.inc"
#include "shaders/widget-multi.vert.inc"
#include "shaders/widget-multi.frag.inc"

/* Creation state for the fixed widget graphics pipeline. The caller owns all
 * returned handles and destroys them after its submissions have completed. */
struct widget_pipeline {
  VkShaderModule vs, fs;
  VkDescriptorSetLayout set_layouts[2];
  VkPipelineLayout layout;
  VkPipeline pipeline;
  VkRenderPass render_pass;
  VkFramebuffer framebuffer;
};

static int widget_pipeline_create(PFN_vkGetInstanceProcAddr gip, VkInstance instance,
    VkDevice device, VkImageView view, int large, int multi,
    VkDescriptorType descriptor_type, int render_family, struct widget_pipeline *out) {
  V(vkCreateShaderModule);
  V(vkCreateDescriptorSetLayout);
  V(vkCreatePipelineLayout);
  V(vkCreateRenderPass);
  V(vkCreateFramebuffer);
  V(vkCreateGraphicsPipelines);
  VkShaderModuleCreateInfo vs_ci = {.sType =
                                        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (multi ? kWidgetMultiVertSpv_word_count : large ? kWidgetLargeVertSpv_word_count : kWidgetVertSpv_word_count) * 4,
                                    .pCode = multi ? kWidgetMultiVertSpv : large ? kWidgetLargeVertSpv : kWidgetVertSpv};
  VkShaderModuleCreateInfo fs_ci = {.sType =
                                        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (multi ? kWidgetMultiFragSpv_word_count : large ? kWidgetLargeFragSpv_word_count : kWidgetFragSpv_word_count) * 4,
                                    .pCode = multi ? kWidgetMultiFragSpv : large ? kWidgetLargeFragSpv : kWidgetFragSpv};
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
  VkDescriptorSetLayout set_layouts[2];
  VkDescriptorSetLayoutBinding multi_bindings[2] = {bind, bind};
  /* Deliberately reverse declaration order. Offsets follow binding number. */
  multi_bindings[0].binding = 3;
  multi_bindings[0].descriptorCount = 2;
  if (multi) { sl_ci.bindingCount = 2; sl_ci.pBindings = multi_bindings; }
  CHECK(p_vkCreateDescriptorSetLayout(device, &sl_ci, NULL, &set_layouts[0]));
  if (multi) {
    /* Lower than set 0 binding 3: set order must take precedence. */
    bind.binding = 1;
    sl_ci.bindingCount = 1; sl_ci.pBindings = &bind;
    CHECK(p_vkCreateDescriptorSetLayout(device, &sl_ci, NULL, &set_layouts[1]));
  }
  VkPipelineLayoutCreateInfo pl_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = multi ? 2 : 1,
      .pSetLayouts = set_layouts};
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
  if (!render_family) CHECK(p_vkCreateRenderPass(device, &rp_ci, NULL, &rp));
  VkFramebufferCreateInfo fb_ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = rp,
                                   .attachmentCount = 1,
                                   .pAttachments = &view,
                                   .width = kWidgetImage,
                                   .height = kWidgetImage,
                                   .layers = 1};
  VkFramebuffer fb = VK_NULL_HANDLE;
  if (!render_family) CHECK(p_vkCreateFramebuffer(device, &fb_ci, NULL, &fb));
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
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo rendering_pipeline = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format};
  if (render_family) gp.pNext = &rendering_pipeline;
  VkPipeline pipeline;
  CHECK(p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL,
                                    &pipeline));
  *out = (struct widget_pipeline){.vs = vs, .fs = fs, .layout = pipeline_layout,
      .pipeline = pipeline, .render_pass = rp, .framebuffer = fb,
      .set_layouts = {set_layouts[0], multi ? set_layouts[1] : VK_NULL_HANDLE}};
  return 0;
}

#endif
