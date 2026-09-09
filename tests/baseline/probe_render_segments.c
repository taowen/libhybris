#include "probe.h"

/* A legal suspend/resume chain with different clears in three segments.
 * Identical attachment info deliberately retains CLEAR on resumed segments:
 * native rendering ignores it, while a lowering must substitute LOAD.
 * Exact readback catches loss of earlier segments without relying on Blender. */
int rendering_segments_probe(int validate, int route, int profile)
{
    void *library = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) return 2;
    PFN_vkGetInstanceProcAddr gip = sym(library, "vkGetInstanceProcAddr");
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = profile ? "Blender" : "hybris-rendering-segments",
        .pEngineName = "Blender", .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .engineVersion = VK_MAKE_VERSION(1, 0, 0), .apiVersion = VK_API_VERSION_1_2};
    struct validation_state validation = {0};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        .pfnUserCallback = validation_message, .pUserData = &validation};
    VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext = &debug, .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (validate) {
        ci.pNext = &features;
        ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer;
        ci.enabledExtensionCount = 2; ci.ppEnabledExtensionNames = extensions;
    }
    CHECK(p_vkCreateInstance(&ci, NULL, &instance));
    V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceFeatures2);
    V(vkGetPhysicalDeviceQueueFamilyProperties); V(vkGetPhysicalDeviceMemoryProperties);
    V(vkEnumerateDeviceExtensionProperties); V(vkCreateDevice); V(vkDestroyDevice);
    V(vkGetDeviceProcAddr); V(vkGetDeviceQueue); V(vkCreateImage); V(vkDestroyImage);
    V(vkGetImageMemoryRequirements); V(vkCreateImageView); V(vkDestroyImageView);
    V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements);
    V(vkAllocateMemory); V(vkFreeMemory); V(vkBindImageMemory); V(vkBindBufferMemory);
    V(vkMapMemory); V(vkUnmapMemory); V(vkInvalidateMappedMemoryRanges);
    V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkResetCommandPool);
    V(vkAllocateCommandBuffers); V(vkBeginCommandBuffer); V(vkEndCommandBuffer);
    V(vkCmdPipelineBarrier); V(vkCmdClearAttachments); V(vkCmdCopyImageToBuffer);
    V(vkCreateFence); V(vkDestroyFence); V(vkResetFences); V(vkWaitForFences); V(vkQueueSubmit);
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    if (validate) {
        V(vkCreateDebugUtilsMessengerEXT);
        destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
        CHECK(p_vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
    }
    uint32_t count = 1, family;
    VkPhysicalDevice physical;
    VkResult enumeration = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((enumeration != VK_SUCCESS && enumeration != VK_INCOMPLETE) || !count) return 2;
    if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, physical, &family)) return 2;
    uint32_t ext_count = 0;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &ext_count, NULL));
    VkExtensionProperties *ext = calloc(ext_count, sizeof(*ext));
    if (ext_count && !ext) return 2;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &ext_count, ext));
    int dynamic_supported = 0;
    for (uint32_t i = 0; i < ext_count; ++i)
        dynamic_supported |= !strcmp(ext[i].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    free(ext);
    VkPhysicalDeviceDynamicRenderingFeatures dynamic = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 available = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dynamic};
    p_vkGetPhysicalDeviceFeatures2(physical, &available);
    if (!dynamic_supported || !dynamic.dynamicRendering) {
        if (messenger) destroy_messenger(instance, messenger, NULL);
        p_vkDestroyInstance(instance, NULL); dlclose(library);
        printf("UNSUPPORTED dynamic rendering\n"); return 3;
    }
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extension = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dynamic,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &device_extension};
    VkDevice device;
    CHECK(p_vkCreateDevice(physical, &dc, NULL, &device));
    probe_mappings("render-segments-device");
    PFN_vkCmdBeginRenderingKHR begin_render = (PFN_vkCmdBeginRenderingKHR)(route == 1 ?
        p_vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR") : route == 2 ?
        (PFN_vkVoidFunction)dlsym(library, "vkCmdBeginRenderingKHR") : gip(instance, "vkCmdBeginRenderingKHR"));
    PFN_vkCmdEndRenderingKHR end_render = (PFN_vkCmdEndRenderingKHR)(route == 1 ?
        p_vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR") : route == 2 ?
        (PFN_vkVoidFunction)dlsym(library, "vkCmdEndRenderingKHR") : gip(instance, "vkCmdEndRenderingKHR"));
    if (!begin_render || !end_render) {
        p_vkDestroyDevice(device, NULL);
        if (messenger) destroy_messenger(instance, messenger, NULL);
        p_vkDestroyInstance(instance, NULL); dlclose(library);
        printf("UNSUPPORTED rendering KHR route=%d\n", route); return 3;
    }
    VkQueue queue; p_vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory; p_vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const uint32_t width = 64, height = 32;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image; CHECK(p_vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements image_req; p_vkGetImageMemoryRequirements(device, image, &image_req);
    uint32_t image_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if (image_req.memoryTypeBits & (1u << i)) { image_type = i; break; }
    if (image_type == UINT32_MAX) return 2;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_req.size, .memoryTypeIndex = image_type};
    VkDeviceMemory image_memory; CHECK(p_vkAllocateMemory(device, &ai, NULL, &image_memory));
    CHECK(p_vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_info.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CHECK(p_vkCreateImageView(device, &view_info, NULL, &view));
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = width * height * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer; CHECK(p_vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements buffer_req; p_vkGetBufferMemoryRequirements(device, buffer, &buffer_req);
    uint32_t host_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((buffer_req.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { host_type = i; break; }
    if (host_type == UINT32_MAX) return 2;
    ai.allocationSize = buffer_req.size; ai.memoryTypeIndex = host_type;
    VkDeviceMemory host_memory; CHECK(p_vkAllocateMemory(device, &ai, NULL, &host_memory));
    CHECK(p_vkBindBufferMemory(device, buffer, host_memory, 0));
    uint8_t *pixels; CHECK(p_vkMapMemory(device, host_memory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool pool; CHECK(p_vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command; CHECK(p_vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; CHECK(p_vkCreateFence(device, &fence_info, NULL, &fence));
    unsigned errors = 0;
    for (unsigned round = 0; round < 4; ++round) {
        CHECK(p_vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(p_vkBeginCommandBuffer(command, &cb));
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = round ? VK_ACCESS_TRANSFER_READ_BIT : 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = round ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        p_vkCmdPipelineBarrier(command, round ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color.float32 = {1, 0, 0, 1}};
        VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {width, height}}, .layerCount = 1,
            .colorAttachmentCount = 1, .pColorAttachments = &attachment};
        for (unsigned segment = 0; segment < 3; ++segment) {
            rendering.flags = (segment ? VK_RENDERING_RESUMING_BIT : 0) |
                              (segment < 2 ? VK_RENDERING_SUSPENDING_BIT : 0);
            begin_render(command, &rendering);
            unsigned shade = (segment + round) % 3;
            VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .clearValue.color.float32 = {shade == 2, shade != 1, shade != 0, 1}};
            VkClearRect rect = {.rect = {{segment ? 32 : 0, segment == 2 ? 16 : 0},
                                         {32, segment ? 16 : 32}}, .layerCount = 1};
            p_vkCmdClearAttachments(command, 1, &clear, 1, &rect);
            end_render(command);
        }
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {width, height, 1}};
        p_vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        VkMemoryBarrier host_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host_barrier, 0, NULL, 0, NULL);
        CHECK(p_vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = host_memory, .size = VK_WHOLE_SIZE};
        CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &range));
        unsigned mismatches = 0;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            unsigned shade = ((x < 32 ? 0 : y < 16 ? 1 : 2) + round) % 3;
            uint8_t expected[] = {shade == 2 ? 255 : 0, shade != 1 ? 255 : 0,
                                 shade != 0 ? 255 : 0, 255};
            mismatches += memcmp(pixels + (y * width + x) * 4, expected, 4) != 0;
        }
        printf("RENDER_SEGMENTS profile=%d route=%d round=%u pixels=%u mismatches=%u\n",
            profile, route, round, width * height, mismatches);
        errors += mismatches;
        CHECK(p_vkResetFences(device, 1, &fence));
    }
    p_vkDestroyFence(device, fence, NULL);
    p_vkDestroyCommandPool(device, pool, NULL);
    p_vkUnmapMemory(device, host_memory);
    p_vkDestroyBuffer(device, buffer, NULL); p_vkFreeMemory(device, host_memory, NULL);
    p_vkDestroyImageView(device, view, NULL);
    p_vkDestroyImage(device, image, NULL); p_vkFreeMemory(device, image_memory, NULL);
    p_vkDestroyDevice(device, NULL);
    if (messenger) destroy_messenger(instance, messenger, NULL);
    p_vkDestroyInstance(instance, NULL); dlclose(library);
    printf("RENDER_SEGMENTS validation_errors=%u\n", validation.errors);
    return errors || validation.errors ? 2 : 0;
}
