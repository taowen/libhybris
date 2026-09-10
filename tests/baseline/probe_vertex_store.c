/* SPDX-License-Identifier: Apache-2.0 */
#include "probe.h"
#include "shaders/point-size.inc"
#include "shaders/vertex-store.inc"

int vertex_store_probe(int raw, int validate)
{
    int features2_request = validate == 2;
    validate = !!validate;
    enum { SIDE = 16, BYTES = SIDE * SIDE * 4 };
    void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 2;
    PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hybris-vertex-store", .apiVersion = VK_API_VERSION_1_1};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    struct validation_state validation = {0};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation_message, .pUserData = &validation};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &sync};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .pNext = validate ? &vf : NULL, .enabledLayerCount = validate, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = validate ? 2 : 0, .ppEnabledExtensionNames = extensions};
    CHECK(p_vkCreateInstance(&ci, NULL, &instance));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    if (validate) {
        V(vkCreateDebugUtilsMessengerEXT);
        destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!destroy_messenger) return 2;
        CHECK(p_vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
    }
    V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceFeatures);
    V(vkGetPhysicalDeviceMemoryProperties); V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkCreateDevice); V(vkDestroyDevice); V(vkGetDeviceQueue); V(vkGetDeviceProcAddr);
    V(vkCreateShaderModule); V(vkDestroyShaderModule); V(vkCreateGraphicsPipelines); V(vkDestroyPipeline);
    V(vkCreatePipelineCache); V(vkDestroyPipelineCache); V(vkCreatePipelineLayout); V(vkDestroyPipelineLayout);
    V(vkCreateImage); V(vkDestroyImage); V(vkGetImageMemoryRequirements); V(vkBindImageMemory);
    V(vkCreateImageView); V(vkDestroyImageView); V(vkAllocateMemory); V(vkFreeMemory);
    V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements); V(vkBindBufferMemory);
    V(vkMapMemory); V(vkUnmapMemory); V(vkCreateRenderPass); V(vkDestroyRenderPass);
    V(vkCreateFramebuffer); V(vkDestroyFramebuffer); V(vkCreateCommandPool); V(vkDestroyCommandPool);
    V(vkAllocateCommandBuffers); V(vkResetCommandPool); V(vkBeginCommandBuffer); V(vkEndCommandBuffer);
    V(vkCmdPipelineBarrier); V(vkCmdBeginRenderPass); V(vkCmdEndRenderPass); V(vkCmdBindPipeline);
    V(vkCreateDescriptorSetLayout); V(vkDestroyDescriptorSetLayout);
    V(vkCreateDescriptorPool); V(vkDestroyDescriptorPool); V(vkAllocateDescriptorSets);
    V(vkUpdateDescriptorSets); V(vkCmdBindDescriptorSets);
    V(vkCmdDrawIndexed); V(vkCmdDrawIndirect); V(vkCmdDrawIndexedIndirect); V(vkCmdBindIndexBuffer);
    V(vkCmdPushConstants); V(vkCmdDraw); V(vkCmdCopyImageToBuffer);
    V(vkCreateFence); V(vkDestroyFence); V(vkResetFences); V(vkWaitForFences); V(vkQueueSubmit);
    uint32_t count = 0;
    CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
    VkPhysicalDevice physical[4];
    if (count > 4) count = 4;
    if (!count) return 3;
    CHECK(p_vkEnumeratePhysicalDevices(instance, &count, physical));
    VkPhysicalDeviceFeatures supported;
    p_vkGetPhysicalDeviceFeatures(physical[0], &supported);
    printf("VERTEX_STORE feature=%u raw_driver_experiment=%d\n", supported.vertexPipelineStoresAndAtomics, raw);
    if (!raw && !supported.vertexPipelineStoresAndAtomics) return 3;
    if (raw && validate) return 2;
    VkPhysicalDeviceFeatures enabled = {.vertexPipelineStoresAndAtomics = raw == 1 ? VK_FALSE : VK_TRUE,
        .shaderClipDistance = supported.shaderClipDistance, .robustBufferAccess = supported.robustBufferAccess, .drawIndirectFirstInstance = supported.drawIndirectFirstInstance};
    if (!supported.drawIndirectFirstInstance) return 3;
    uint32_t queue_count = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &queue_count, NULL);
    VkQueueFamilyProperties *families = calloc(queue_count, sizeof(*families));
    if (!families) return 2;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &queue_count, families);
    uint32_t family = 0;
    while (family < queue_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    free(families);
    if (family == queue_count) return 3;
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qc, .pEnabledFeatures = &enabled};
    VkPhysicalDeviceFeatures2 requested = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .features = enabled};
    VkPhysicalDevice16BitStorageFeatures prefix = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .pNext = &requested};
    if (features2_request) { dc.pNext = &prefix; dc.pEnabledFeatures = NULL; }
    VkDevice device;
    CHECK(p_vkCreateDevice(physical[0], &dc, NULL, &device));
    if (memcmp(&requested.features, &enabled, sizeof(enabled)) || prefix.pNext != &requested) {
        fprintf(stderr, "VERTEX_STORE device creation modified caller features\n");
        return 2;
    }
    VkQueue queue;
    p_vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory;
    p_vkGetPhysicalDeviceMemoryProperties(physical[0], &memory);
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {SIDE, SIDE, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image;
    CHECK(p_vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements requirements;
    p_vkGetImageMemoryRequirements(device, image, &requirements);
    int index = find_mem(&memory, requirements.memoryTypeBits, 0);
    if (index < 0) return 3;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = index};
    VkDeviceMemory image_memory, buffer_memory;
    CHECK(p_vkAllocateMemory(device, &allocation, NULL, &image_memory));
    CHECK(p_vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(p_vkCreateImageView(device, &view_info, NULL, &view));
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    CHECK(p_vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    p_vkGetBufferMemoryRequirements(device, buffer, &requirements);
    index = find_mem(&memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (index < 0) return 3;
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = index;
    CHECK(p_vkAllocateMemory(device, &allocation, NULL, &buffer_memory));
    CHECK(p_vkBindBufferMemory(device, buffer, buffer_memory, 0));
    uint32_t *pixels;
    CHECK(p_vkMapMemory(device, buffer_memory, 0, BYTES, 0, (void **)&pixels));
    VkAttachmentDescription attachment = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkSubpassDependency dependency = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 1, .pDependencies = &dependency};
    VkRenderPass pass;
    CHECK(p_vkCreateRenderPass(device, &pass_info, NULL, &pass));
    VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass, .attachmentCount = 1, .pAttachments = &view, .width = SIDE, .height = SIDE, .layers = 1};
    VkFramebuffer framebuffer;
    CHECK(p_vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
    enum { WORDS = 4 + 32 * 4, GUARD = 64, TOTAL = WORDS + 2 * GUARD };
    VkBuffer storage;
    VkDeviceMemory storage_memory;
    buffer_info.size = TOTAL * 4; buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    CHECK(p_vkCreateBuffer(device, &buffer_info, NULL, &storage));
    p_vkGetBufferMemoryRequirements(device, storage, &requirements);
    index = find_mem(&memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (index < 0) return 3;
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = index;
    CHECK(p_vkAllocateMemory(device, &allocation, NULL, &storage_memory));
    CHECK(p_vkBindBufferMemory(device, storage, storage_memory, 0));
    uint32_t *records;
    CHECK(p_vkMapMemory(device, storage_memory, 0, TOTAL * 4, 0, (void **)&records));
    /* Sentinel indices around a sparse indexed draw catch speculative hole
     * execution. Both indirect commands also exercise nonzero firstInstance. */
    VkBuffer arguments;
    VkDeviceMemory argument_memory;
    buffer_info.size = 128; buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    CHECK(p_vkCreateBuffer(device, &buffer_info, NULL, &arguments));
    p_vkGetBufferMemoryRequirements(device, arguments, &requirements);
    index = find_mem(&memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (index < 0) return 3;
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = index;
    CHECK(p_vkAllocateMemory(device, &allocation, NULL, &argument_memory));
    CHECK(p_vkBindBufferMemory(device, arguments, argument_memory, 0));
    uint8_t *argument_bytes;
    CHECK(p_vkMapMemory(device, argument_memory, 0, 128, 0, (void **)&argument_bytes));
    const uint32_t indices[] = {0x7fffffffu, 5, 9, 17, 0x7fffffffu};
    memcpy(argument_bytes, indices, sizeof(indices));
    VkDrawIndirectCommand direct = {3, 2, 7, 5};
    VkDrawIndexedIndirectCommand indexed = {3, 2, 1, 2, 5};
    memcpy(argument_bytes + 32, &direct, sizeof(direct));
    memcpy(argument_bytes + 64, &indexed, sizeof(indexed));
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo descriptor_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout descriptor_layout;
    CHECK(p_vkCreateDescriptorSetLayout(device, &descriptor_info, NULL, &descriptor_layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo descriptor_pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool descriptor_pool;
    CHECK(p_vkCreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo descriptor_alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &descriptor_layout};
    VkDescriptorSet descriptor;
    CHECK(p_vkAllocateDescriptorSets(device, &descriptor_alloc, &descriptor));
    VkDescriptorBufferInfo storage_info = {storage, GUARD * 4, WORDS * 4};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storage_info};
    p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(p_vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineCacheCreateInfo cache_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;
    CHECK(p_vkCreatePipelineCache(device, &cache_info, NULL, &cache));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(p_vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(p_vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(p_vkCreateFence(device, &fence_info, NULL, &fence));
    VkViewport viewport = {0, 0, SIDE, SIDE, 0, 1};
    VkRect2D scissor = {{0, 0}, {SIDE, SIDE}};
    VkPipelineViewportStateCreateInfo viewport_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState color = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color};
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main"}};
    VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_info, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &blend, .layout = layout, .renderPass = pass};
    const uint32_t *vertices[] = {kVertexStore};
    const size_t sizes[] = {sizeof(kVertexStore)};
    unsigned failures = 0, readbacks = 0;
    for (unsigned variant = 0; variant < 1; ++variant) {
        VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizes[variant], .pCode = vertices[variant]};
        CHECK(p_vkCreateShaderModule(device, &shader_info, NULL, &stages[0].module));
        shader_info.codeSize = sizeof(kPointFragment); shader_info.pCode = kPointFragment;
        CHECK(p_vkCreateShaderModule(device, &shader_info, NULL, &stages[1].module));
        for (unsigned draw = 0; draw < 4; ++draw) for (unsigned round = 0; round < 3; ++round) {
            for (unsigned i = 0; i < TOTAL; ++i) records[i] = 0xdeadbeefu;
            records[GUARD] = records[GUARD + 1] = 0;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            raster.rasterizerDiscardEnable = round == 2;
            pipeline_info.stageCount = round == 2 ? 1 : 2;
            pipeline_info.pViewportState = round == 2 ? NULL : &viewport_info;
            pipeline_info.pMultisampleState = round == 2 ? NULL : &samples;
            pipeline_info.pColorBlendState = round == 2 ? NULL : &blend;
            VkPipeline pipeline;
            CHECK(p_vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, NULL, &pipeline));
            CHECK(p_vkResetCommandPool(device, pool, 0));
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(p_vkBeginCommandBuffer(command, &begin));
            VkMemoryBarrier reuse = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &reuse, 0, NULL, 0, NULL);
            VkClearValue clear = {.color = {.float32 = {0, 0, 0, 1}}};
            VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = pass, .framebuffer = framebuffer, .renderArea = scissor,
                .clearValueCount = 1, .pClearValues = &clear};
            p_vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
            p_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            uint32_t mode = round | ((draw & 1) ? 256 : 0);
            p_vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &mode);
            p_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptor, 0, NULL);
            if (draw & 1) p_vkCmdBindIndexBuffer(command, arguments, 0, VK_INDEX_TYPE_UINT32);
            if (draw == 0) p_vkCmdDraw(command, 3, 2, 7, 5);
            if (draw == 1) p_vkCmdDrawIndexed(command, 3, 2, 1, 2, 5);
            if (draw == 2) p_vkCmdDrawIndirect(command, arguments, 32, 1, sizeof(direct));
            if (draw == 3) p_vkCmdDrawIndexedIndirect(command, arguments, 64, 1, sizeof(indexed));
            p_vkCmdEndRenderPass(command);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {SIDE, SIDE, 1}};
            p_vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
            CHECK(p_vkEndCommandBuffer(command));
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
            CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
            CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
            unsigned bad = 0, white = 0;
            for (unsigned y = 0; y < SIDE; ++y) for (unsigned x = 0; x < SIDE; ++x) {
                int lit = round == 0;
                uint32_t expected = lit ? 0xffffffff : 0xff000000;
                if (pixels[y * SIDE + x] != expected) ++bad;
                white += pixels[y * SIDE + x] == 0xffffffff;
            }
            unsigned record_bad = 0;
            for (unsigned i = 0; i < TOTAL; ++i) {
                uint32_t expected = 0xdeadbeefu;
                if (i == GUARD) expected = records[i] >= 6 ? records[i] : 6;
                else if (i == GUARD + 1) expected = 63;
                else if (i >= GUARD + 4 && i < GUARD + WORDS) {
                    unsigned record = (i - GUARD - 4) / 4, component = (i - GUARD - 4) % 4;
                    unsigned inst = record / 16, vertex = record % 16;
                    int written = (draw & 1) ? (vertex == 0 || vertex == 4 || vertex == 12) : vertex < 3;
                    if (written) expected = component == 0 ? vertex + 7 : component == 1 ? inst + 5 : component == 2 ? mode : 0xabcdefu;
                }
                if (records[i] != expected) {
                    if (record_bad < 8) printf("VERTEX_STORE_MISMATCH word=%u expected=%08x actual=%08x\n", i, expected, records[i]);
                    ++record_bad;
                }
            }
            printf("VERTEX_STORE_READBACK draw=%u round=%u white=%u pixel_bad=%u count=%u mask=%u record_bad=%u\n",
                draw, round, white, bad, records[GUARD], records[GUARD + 1], record_bad);
            bad += record_bad;
            failures += bad; ++readbacks;
            CHECK(p_vkResetFences(device, 1, &fence));
            p_vkDestroyPipeline(device, pipeline, NULL);
        }
        p_vkDestroyShaderModule(device, stages[0].module, NULL);
        p_vkDestroyShaderModule(device, stages[1].module, NULL);
    }
    p_vkUnmapMemory(device, argument_memory); p_vkDestroyBuffer(device, arguments, NULL); p_vkFreeMemory(device, argument_memory, NULL);
    p_vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    p_vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    p_vkUnmapMemory(device, storage_memory); p_vkDestroyBuffer(device, storage, NULL); p_vkFreeMemory(device, storage_memory, NULL);
    p_vkDestroyFence(device, fence, NULL); p_vkDestroyCommandPool(device, pool, NULL);
    p_vkDestroyPipelineCache(device, cache, NULL); p_vkDestroyPipelineLayout(device, layout, NULL);
    p_vkDestroyFramebuffer(device, framebuffer, NULL); p_vkDestroyRenderPass(device, pass, NULL);
    p_vkUnmapMemory(device, buffer_memory); p_vkDestroyBuffer(device, buffer, NULL); p_vkFreeMemory(device, buffer_memory, NULL);
    p_vkDestroyImageView(device, view, NULL); p_vkDestroyImage(device, image, NULL); p_vkFreeMemory(device, image_memory, NULL);
    p_vkDestroyDevice(device, NULL);
    if (messenger) destroy_messenger(instance, messenger, NULL);
    p_vkDestroyInstance(instance, NULL); dlclose(h);
    printf("VERTEX_STORE_SUMMARY readbacks=%u failures=%u validation_errors=%u raw=%d features2=%d\n", readbacks, failures, validation.errors, raw, features2_request);
    return failures || validation.errors || readbacks != 12 ? 2 : 0;
}
