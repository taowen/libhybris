/* SPDX-License-Identifier: Apache-2.0 */
#include "probe.h"
#include "shaders/point-size.inc"

int point_size_probe(int validate, int route)
{
    enum { SIDE = 16, BYTES = SIDE * SIDE * 4 };
    void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 2;
    PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hybris-point-size", .apiVersion = VK_API_VERSION_1_1};
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
    if (!supported.largePoints) { printf("POINT_SIZE_UNSUPPORTED largePoints=0\n"); return 3; }
    VkPhysicalDeviceFeatures enabled = {.largePoints = VK_TRUE};
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
    VkDevice device;
    CHECK(p_vkCreateDevice(physical[0], &dc, NULL, &device));
#ifdef HYBRIS_PROBE_LINKED
#define LINKED(n) n
#else
#define LINKED(n) NULL
#endif
#define ROUTE(n) do { if (route == 1) p_##n = (PFN_##n)p_vkGetDeviceProcAddr(device, #n); \
    else if (route == 2) p_##n = (PFN_##n)sym(h, #n); else if (route == 3) p_##n = LINKED(n); \
    if (!p_##n) return 2; } while (0)
    ROUTE(vkCreateShaderModule); ROUTE(vkDestroyShaderModule); ROUTE(vkCreateGraphicsPipelines);
#undef ROUTE
#undef LINKED
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
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
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
    const uint32_t *vertices[] = {kPointVertex, kPointRead, kPointMixed};
    const size_t sizes[] = {sizeof(kPointVertex), sizeof(kPointRead), sizeof(kPointMixed)};
    unsigned failures = 0, readbacks = 0;
    for (unsigned variant = 0; variant < 3; ++variant) {
        VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizes[variant], .pCode = vertices[variant]};
        CHECK(p_vkCreateShaderModule(device, &shader_info, NULL, &stages[0].module));
        shader_info.codeSize = sizeof(kPointFragment); shader_info.pCode = kPointFragment;
        CHECK(p_vkCreateShaderModule(device, &shader_info, NULL, &stages[1].module));
        for (unsigned round = 0; round < 3; ++round) {
            int point = round == 1;
            assembly.topology = point ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipeline pipeline;
            CHECK(p_vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, NULL, &pipeline));
            CHECK(p_vkResetCommandPool(device, pool, 0));
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(p_vkBeginCommandBuffer(command, &begin));
            VkMemoryBarrier reuse = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0, NULL, 0, NULL);
            VkClearValue clear = {.color = {.float32 = {0, 0, 0, 1}}};
            VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .renderPass = pass, .framebuffer = framebuffer, .renderArea = scissor,
                .clearValueCount = 1, .pClearValues = &clear};
            p_vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
            p_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            uint32_t mode = point ? (variant == 2 ? 2 : 1) : 0;
            p_vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &mode);
            p_vkCmdDraw(command, point ? 1 : 3, 1, 0, 0);
            p_vkCmdEndRenderPass(command);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {SIDE, SIDE, 1}};
            p_vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
            CHECK(p_vkEndCommandBuffer(command));
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
            CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
            CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
            unsigned bad = 0, white = 0, radius = variant == 2 ? 2 : 0;
            for (unsigned y = 0; y < SIDE; ++y) for (unsigned x = 0; x < SIDE; ++x) {
                int lit = !point || (x >= 8 - radius && x <= 8 + radius && y >= 8 - radius && y <= 8 + radius);
                uint32_t expected = lit ? 0xffffffff : 0xff000000;
                if (pixels[y * SIDE + x] != expected) ++bad;
                white += pixels[y * SIDE + x] == 0xffffffff;
            }
            printf("POINT_SIZE_READBACK variant=%u round=%u topology=%u white=%u bad=%u\n", variant, round, assembly.topology, white, bad);
            failures += bad; ++readbacks;
            CHECK(p_vkResetFences(device, 1, &fence));
            p_vkDestroyPipeline(device, pipeline, NULL);
        }
        p_vkDestroyShaderModule(device, stages[0].module, NULL);
        p_vkDestroyShaderModule(device, stages[1].module, NULL);
    }
    p_vkDestroyFence(device, fence, NULL); p_vkDestroyCommandPool(device, pool, NULL);
    p_vkDestroyPipelineCache(device, cache, NULL); p_vkDestroyPipelineLayout(device, layout, NULL);
    p_vkDestroyFramebuffer(device, framebuffer, NULL); p_vkDestroyRenderPass(device, pass, NULL);
    p_vkUnmapMemory(device, buffer_memory); p_vkDestroyBuffer(device, buffer, NULL); p_vkFreeMemory(device, buffer_memory, NULL);
    p_vkDestroyImageView(device, view, NULL); p_vkDestroyImage(device, image, NULL); p_vkFreeMemory(device, image_memory, NULL);
    p_vkDestroyDevice(device, NULL);
    if (messenger) destroy_messenger(instance, messenger, NULL);
    p_vkDestroyInstance(instance, NULL); dlclose(h);
    printf("POINT_SIZE_SUMMARY readbacks=%u failures=%u validation_errors=%u route=%d\n", readbacks, failures, validation.errors, route);
    return failures || validation.errors || readbacks != 9 ? 2 : 0;
}
