#include "probe.h"
#include "bc_image_verify.h"
#define BC_IMAGE_FORMAT_COUNT BC_FORMAT_COUNT
#include "shaders/bc-sample.inc"

static void image_barrier(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer command, VkImage image,
    uint32_t mip, uint32_t layers, VkImageLayout before, VkImageLayout after,
    VkPipelineStageFlags source_stage, VkAccessFlags source_access,
    VkPipelineStageFlags destination_stage, VkAccessFlags destination_access)
{
    VkImageMemoryBarrier info = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = source_access, .dstAccessMask = destination_access,
        .oldLayout = before, .newLayout = after, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, layers}};
    barrier(command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &info);
}
static void image_event(PFN_vkCmdSetEvent set, PFN_vkCmdWaitEvents wait,
    PFN_vkCmdSetEvent2 set2, PFN_vkCmdWaitEvents2 wait2, VkCommandBuffer command,
    VkEvent event, VkImage image, uint32_t mip, uint32_t layers, int sync2)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, layers}};
    if (sync2) {
        VkImageMemoryBarrier2 extended = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = barrier.srcAccessMask,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = barrier.dstAccessMask,
            .oldLayout = barrier.oldLayout, .newLayout = barrier.newLayout,
            .srcQueueFamilyIndex = barrier.srcQueueFamilyIndex, .dstQueueFamilyIndex = barrier.dstQueueFamilyIndex,
            .image = image, .subresourceRange = barrier.subresourceRange};
        VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &extended};
        set2(command, event, &dependency);
        wait2(command, 1, &event, &dependency);
    } else {
        set(command, event, VK_PIPELINE_STAGE_TRANSFER_BIT);
        wait(command, 1, &event, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, NULL, 0, NULL, 1, &barrier);
    }
}
int bc_images_probe(int validate, int route)
{
    void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 2;
    PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    struct validation_state validation = {0};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = validation_message, .pUserData = &validation};
    VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .pNext = validate ? &vf : NULL, .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = validate ? 2 : 0, .ppEnabledExtensionNames = extensions};
    CHECK(p_vkCreateInstance(&ci, NULL, &instance));
    V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceProperties);
    V(vkGetPhysicalDeviceMemoryProperties); V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkGetPhysicalDeviceFormatProperties); V(vkGetPhysicalDeviceFormatProperties2);
    V(vkGetPhysicalDeviceImageFormatProperties); V(vkGetPhysicalDeviceImageFormatProperties2);
    V(vkGetPhysicalDeviceSparseImageFormatProperties); V(vkGetPhysicalDeviceSparseImageFormatProperties2);
    V(vkEnumerateDeviceExtensionProperties); V(vkGetPhysicalDeviceFeatures); V(vkGetPhysicalDeviceFeatures2);
    V(vkCreateDevice); V(vkGetDeviceProcAddr);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count) return 2;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceProperties(physical, &properties);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    VkPhysicalDeviceFeatures features;
    p_vkGetPhysicalDeviceFeatures(physical, &features);
    printf("BC_IMAGES_DEVICE api=%u compression_bc=%u pipeline_statistics=%u\n",
        properties.apiVersion, features.textureCompressionBC, features.pipelineStatisticsQuery);
    uint32_t families = 0, family = UINT32_MAX;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties *queues = calloc(families, sizeof(*queues));
    if (!queues) return 2;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues);
    for (uint32_t i = 0; i < families; ++i) {
        printf("BC_IMAGES_QUEUE family=%u flags=%u granularity=%u,%u,%u\n", i, queues[i].queueFlags,
            queues[i].minImageTransferGranularity.width, queues[i].minImageTransferGranularity.height,
            queues[i].minImageTransferGranularity.depth);
        if (family == UINT32_MAX && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) family = i;
    }
    free(queues);
    if (family == UINT32_MAX) return 3;
    VkBool32 linear_filter[BC_IMAGE_FORMAT_COUNT] = {0};
    unsigned unsupported_formats = 0;
    for (unsigned f = 0; f < BC_IMAGE_FORMAT_COUNT; ++f) {
        VkFormatProperties format;
        p_vkGetPhysicalDeviceFormatProperties(physical, bc_formats[f], &format);
        VkFormatProperties2 format2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        p_vkGetPhysicalDeviceFormatProperties2(physical, bc_formats[f], &format2);
        if (memcmp(&format, &format2.formatProperties, sizeof(format))) return 2;
        VkImageFormatProperties image;
        result = p_vkGetPhysicalDeviceImageFormatProperties(physical, bc_formats[f], VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image);
        printf("BC_IMAGES_FORMAT format=%u optimal=%u image_result=%d\n", bc_formats[f], format.optimalTilingFeatures, result);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED || !(format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            ++unsupported_formats;
            continue;
        }
        if (result != VK_SUCCESS) return 2;
        linear_filter[f] = (f < 2 || f >= 8) && (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        const char *bc_policy = getenv("HYBRIS_BC_TEXTURES");
        if (bc_policy && !strcmp(bc_policy, "force")) {
            uint32_t sparse_count = 0;
            p_vkGetPhysicalDeviceSparseImageFormatProperties(physical, bc_formats[f], VK_IMAGE_TYPE_2D,
                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &sparse_count, NULL);
            if (sparse_count) return 2;
            VkPhysicalDeviceSparseImageFormatInfo2 sparse = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,
                .format = bc_formats[f], .type = VK_IMAGE_TYPE_2D, .samples = VK_SAMPLE_COUNT_1_BIT,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL};
            p_vkGetPhysicalDeviceSparseImageFormatProperties2(physical, &sparse, &sparse_count, NULL);
            if (sparse_count) return 2;
        }
        VkPhysicalDeviceImageFormatInfo2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = bc_formats[f], .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImageFormatProperties2 image2 = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        CHECK(p_vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &image2));
        if (memcmp(&image, &image2.imageFormatProperties, sizeof(image))) return 2;
    }
    if (unsupported_formats) {
        p_vkDestroyInstance(instance, NULL); dlclose(h);
        printf("UNSUPPORTED bc-images missing_formats=%u\n", unsupported_formats);
        return 3;
    }
    uint32_t extension_count = 0;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL));
    VkExtensionProperties *available = calloc(extension_count, sizeof(*available));
    if (!available) return 2;
    CHECK(p_vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, available));
    int copies2 = 0, sync2 = 0, maintenance4 = 0, format_list = 0;
    for (uint32_t i = 0; i < extension_count; ++i) {
        if (!strcmp(available[i].extensionName, VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME)) copies2 = 1;
        if (!strcmp(available[i].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) sync2 = 1;
        if (!strcmp(available[i].extensionName, VK_KHR_MAINTENANCE_4_EXTENSION_NAME)) maintenance4 = 1;
        if (!strcmp(available[i].extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) format_list = 1;
    }
    free(available);
    VkPhysicalDeviceSynchronization2Features synchronization = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    if (sync2) {
        VkPhysicalDeviceFeatures2 queried = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &synchronization};
        p_vkGetPhysicalDeviceFeatures2(physical, &queried);
        sync2 = synchronization.synchronization2;
    }
    VkPhysicalDeviceMaintenance4Features maintenance = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    if (maintenance4) {
        VkPhysicalDeviceFeatures2 queried = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &maintenance};
        p_vkGetPhysicalDeviceFeatures2(physical, &queried);
        maintenance4 = maintenance.maintenance4;
    }
    synchronization.pNext = maintenance4 ? &maintenance : NULL;
    const char *device_extensions[4];
    uint32_t device_extension_count = 0;
    if (copies2) device_extensions[device_extension_count++] = VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME;
    if (sync2) device_extensions[device_extension_count++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    if (maintenance4) device_extensions[device_extension_count++] = VK_KHR_MAINTENANCE_4_EXTENSION_NAME;
    if (format_list) device_extensions[device_extension_count++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc,
        .pNext = sync2 ? (void *)&synchronization : maintenance4 ? (void *)&maintenance : NULL,
        .enabledExtensionCount = device_extension_count, .ppEnabledExtensionNames = device_extensions};
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    if (validate) {
        V(vkCreateDebugUtilsMessengerEXT);
        destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!destroy_messenger) return 2;
        CHECK(p_vkCreateDebugUtilsMessengerEXT(instance, &debug, NULL, &messenger));
    }
    VkDevice device;
    CHECK(p_vkCreateDevice(physical, &dc, NULL, &device));
#ifdef HYBRIS_PROBE_LINKED
#define BC_LINKED(name) name
#else
#define BC_LINKED(name) NULL
#endif
#define BC_RESOLVE(name) (PFN_##name)(route == 1 ? p_vkGetDeviceProcAddr(device, #name) : \
    route == 2 ? (PFN_vkVoidFunction)sym(h, #name) : route == 3 ? (PFN_vkVoidFunction)BC_LINKED(name) : gip(instance, #name))
#define D(name) PFN_##name p_##name = BC_RESOLVE(name); \
    if (!p_##name) { printf("MISSING %s\n", #name); return 2; }
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkDestroyBuffer); D(vkGetBufferMemoryRequirements);
    D(vkCreateImage); D(vkDestroyImage); D(vkGetImageMemoryRequirements); D(vkGetImageMemoryRequirements2);
    D(vkAllocateMemory); D(vkFreeMemory); D(vkBindImageMemory2); D(vkBindBufferMemory);
    D(vkMapMemory); D(vkUnmapMemory); D(vkFlushMappedMemoryRanges); D(vkInvalidateMappedMemoryRanges);
    D(vkCreateImageView); D(vkDestroyImageView); D(vkCreateSampler); D(vkDestroySampler);
    D(vkCreateDescriptorSetLayout); D(vkDestroyDescriptorSetLayout); D(vkCreateDescriptorPool);
    D(vkDestroyDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets);
    D(vkCreateShaderModule); D(vkDestroyShaderModule); D(vkCreatePipelineLayout); D(vkDestroyPipelineLayout);
    D(vkCreateComputePipelines); D(vkDestroyPipeline); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets);
    D(vkCmdPushConstants); D(vkCmdDispatch); D(vkCmdCopyBufferToImage); D(vkCmdCopyImage); D(vkCmdCopyImageToBuffer);
    D(vkCreateCommandPool); D(vkDestroyCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkResetCommandBuffer); D(vkResetCommandPool); D(vkFreeCommandBuffers);
    D(vkCmdFillBuffer); D(vkCmdPipelineBarrier);
    D(vkCreateEvent); D(vkDestroyEvent); D(vkResetEvent); D(vkCmdSetEvent); D(vkCmdWaitEvents);
    D(vkCreateFence); D(vkDestroyFence); D(vkWaitForFences); D(vkResetFences); D(vkQueueSubmit);
#undef D
    /* Standard Vulkan loaders need not export extension aliases as ELF
     * symbols. Test those through GIPA/GDPA even in the core ELF cases. */
#define BC_EXTENSION(name) (PFN_##name)(route == 0 ? gip(instance, #name) : p_vkGetDeviceProcAddr(device, #name))
    PFN_vkCmdCopyBufferToImage2 copy_to_image2 = copies2 ? (PFN_vkCmdCopyBufferToImage2)
        BC_EXTENSION(vkCmdCopyBufferToImage2KHR) : NULL;
    PFN_vkCmdCopyImage2 copy_image2 = copies2 ? (PFN_vkCmdCopyImage2)
        BC_EXTENSION(vkCmdCopyImage2KHR) : NULL;
    PFN_vkCmdCopyImageToBuffer2 copy_to_buffer2 = copies2 ? (PFN_vkCmdCopyImageToBuffer2)
        BC_EXTENSION(vkCmdCopyImageToBuffer2KHR) : NULL;
    if (copies2 && (!copy_to_image2 || !copy_image2 || !copy_to_buffer2)) return 2;
    PFN_vkCmdSetEvent2 set_event2 = sync2 ? (PFN_vkCmdSetEvent2)BC_EXTENSION(vkCmdSetEvent2KHR) : NULL;
    PFN_vkCmdWaitEvents2 wait_events2 = sync2 ? (PFN_vkCmdWaitEvents2)BC_EXTENSION(vkCmdWaitEvents2KHR) : NULL;
    PFN_vkCmdPipelineBarrier2 pipeline_barrier2 = sync2 ?
        (PFN_vkCmdPipelineBarrier2)BC_EXTENSION(vkCmdPipelineBarrier2KHR) : NULL;
    if (sync2 && (!set_event2 || !wait_events2 || !pipeline_barrier2)) return 2;
    PFN_vkGetDeviceImageMemoryRequirements device_requirements = maintenance4 ?
        (PFN_vkGetDeviceImageMemoryRequirements)BC_EXTENSION(vkGetDeviceImageMemoryRequirementsKHR) : NULL;
    if (maintenance4 && !device_requirements) return 2;
#undef BC_EXTENSION
#undef BC_RESOLVE
#undef BC_LINKED
    VkEventCreateInfo event_info = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent event;
    CHECK(p_vkCreateEvent(device, &event_info, NULL, &event));
    VkQueue queue;
    p_vkGetDeviceQueue(device, family, 0, &queue);
    enum { BYTES = 524288, RAW = 393216, REFERENCE = 65536, OUTPUT = 262144, LAYERS = 3 };
    VkBuffer buffers[2]; VkDeviceMemory buffer_memory[2]; VkDeviceSize binding[2]; uint8_t *mapped[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                (i ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(p_vkCreateBuffer(device, &bi, NULL, &buffers[i]));
        VkMemoryRequirements req;
        p_vkGetBufferMemoryRequirements(device, buffers[i], &req);
        int type = find_mem(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (type < 0) return 3;
        binding[i] = req.alignment;
        VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size + binding[i], .memoryTypeIndex = type};
        CHECK(p_vkAllocateMemory(device, &allocation, NULL, &buffer_memory[i]));
        CHECK(p_vkBindBufferMemory(device, buffers[i], buffer_memory[i], binding[i]));
        CHECK(p_vkMapMemory(device, buffer_memory[i], 0, VK_WHOLE_SIZE, 0, (void **)&mapped[i]));
    }
    uint32_t dynamic_offset = properties.limits.minStorageBufferOffsetAlignment;
    if (dynamic_offset < 256) dynamic_offset = 256;
    if (dynamic_offset + OUTPUT > RAW) return 3;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout descriptor_layout;
    CHECK(p_vkCreateDescriptorSetLayout(device, &layout_info, NULL, &descriptor_layout));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
    VkDescriptorPoolCreateInfo descriptor_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool descriptor_pool;
    CHECK(p_vkCreateDescriptorPool(device, &descriptor_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &descriptor_layout};
    VkDescriptorSet descriptors;
    CHECK(p_vkAllocateDescriptorSets(device, &set_info, &descriptors));
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSampler samplers[5];
    CHECK(p_vkCreateSampler(device, &sampler_info, NULL, &samplers[0]));
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    CHECK(p_vkCreateSampler(device, &sampler_info, NULL, &samplers[1]));
    sampler_info.addressModeU = sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    CHECK(p_vkCreateSampler(device, &sampler_info, NULL, &samplers[2]));
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    CHECK(p_vkCreateSampler(device, &sampler_info, NULL, &samplers[3]));
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    CHECK(p_vkCreateSampler(device, &sampler_info, NULL, &samplers[4]));
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(bc_sample_spv), .pCode = bc_sample_spv};
    VkShaderModule shader;
    CHECK(p_vkCreateShaderModule(device, &shader_info, NULL, &shader));
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(p_vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(p_vkAllocateCommandBuffers(device, &command_info, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(p_vkCreateFence(device, &fence_info, NULL, &fence));
    unsigned failures = 0, readbacks = 0, copies2_cases = 0, sync2_cases = 0;
    for (unsigned f = 0; f < BC_IMAGE_FORMAT_COUNT; ++f) {
        for (unsigned shape_index = 0; shape_index < 2; ++shape_index) {
        uint32_t base_width = shape_index ? 32 : 9, base_height = shape_index ? 32 : 7;
        for (uint32_t mip = 0; mip < 4; ++mip) {
            uint32_t width = base_width >> mip, height = base_height >> mip;
            if (!width) width = 1;
            if (!height) height = 1;
            int use2 = copies2 && (mip & 1);
            copies2_cases += use2;
            int use_sync2 = sync2 && (mip & 1);
            sync2_cases += use_sync2;
            unsigned mode = bc_mode(f);
            unsigned sampler_choice = linear_filter[f] ? 1 + (f < 2 || f >= 12 ? mip : mip % 3) : 0;
            /* Legacy opaque-black sampling with nonidentity swizzles is undefined. */
            unsigned view_swizzled = shape_index && sampler_choice != 4;
            uint32_t block_bytes = bc_block_bytes(mode);
            VkImage images[4]; VkDeviceMemory image_memory[4];
            for (unsigned i = 0; i < 4; ++i) {
                VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = i == 2 && shape_index ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = i == 2 ?
                        (block_bytes == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT) :
                        i == 3 ? bc_reference_format(f) : bc_formats[f],
                    .extent = i == 2 ? (VkExtent3D){(width + 3) / 4, (height + 3) / 4, shape_index ? LAYERS : 1} :
                        i == 3 ? (VkExtent3D){width, height, 1} : (VkExtent3D){base_width, base_height, 1},
                    .mipLevels = i >= 2 ? 1 : 4, .arrayLayers = i == 2 && shape_index ? 1 : LAYERS, .samples = VK_SAMPLE_COUNT_1_BIT,
                    .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        (i != 2 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
                VkImageFormatListCreateInfo formats = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                    .viewFormatCount = 1, .pViewFormats = &image_info.format};
                if (format_list) image_info.pNext = &formats;
                VkMemoryRequirements2 described = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
                if (maintenance4) {
                    VkDeviceImageMemoryRequirements description = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                        .pCreateInfo = &image_info};
                    device_requirements(device, &description, &described);
                }
                CHECK(p_vkCreateImage(device, &image_info, NULL, &images[i]));
                VkImageMemoryRequirementsInfo2 request = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = images[i]};
                VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
                p_vkGetImageMemoryRequirements2(device, &request, &requirements);
                VkMemoryRequirements legacy;
                p_vkGetImageMemoryRequirements(device, images[i], &legacy);
                if (legacy.size != requirements.memoryRequirements.size ||
                    legacy.alignment != requirements.memoryRequirements.alignment ||
                    legacy.memoryTypeBits != requirements.memoryRequirements.memoryTypeBits) return 2;
                if (maintenance4 && (legacy.size != described.memoryRequirements.size ||
                    legacy.alignment != described.memoryRequirements.alignment ||
                    legacy.memoryTypeBits != described.memoryRequirements.memoryTypeBits)) {
                    printf("BC_IMAGES_MEMORY_QUERY_MISMATCH format=%u mip=%u image=%u\n", bc_formats[f], mip, i);
                    return 2;
                }
                int type = find_mem(&mp, legacy.memoryTypeBits, 0);
                if (type < 0) return 3;
                int own = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation || (mip & 1);
                VkDeviceSize offset = own ? 0 : legacy.alignment;
                VkMemoryDedicatedAllocateInfo owner = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = images[i]};
                VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = own ? &owner : NULL};
                VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
                    .allocationSize = legacy.size + offset, .memoryTypeIndex = type};
                CHECK(p_vkAllocateMemory(device, &allocation, NULL, &image_memory[i]));
                uint32_t index = 0;
                VkBindImageMemoryDeviceGroupInfo group = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO,
                    .deviceIndexCount = 1, .pDeviceIndices = &index};
                VkBindImageMemoryInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                    .pNext = &group, .image = images[i], .memory = image_memory[i], .memoryOffset = offset};
                CHECK(p_vkBindImageMemory2(device, 1, &bind));
            }
            VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = images[1], .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = bc_formats[f],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, LAYERS}};
            if (view_swizzled) view_info.components = (VkComponentMapping){
                VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
            VkImageView view;
            CHECK(p_vkCreateImageView(device, &view_info, NULL, &view));
            view_info.image = images[3];
            view_info.format = bc_reference_format(f);
            /* BC1 RGB and BC6H have no alpha, including outside the image. */
            if ((f < 2 || f >= 14) && sampler_choice != 4) view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
            view_info.subresourceRange.baseMipLevel = 0;
            VkImageView reference_view;
            CHECK(p_vkCreateImageView(device, &view_info, NULL, &reference_view));
            VkDescriptorImageInfo reference = {.sampler = samplers[sampler_choice], .imageView = reference_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo sampled = {.sampler = samplers[sampler_choice], .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorBufferInfo output = {.buffer = buffers[1], .range = OUTPUT};
            VkWriteDescriptorSet writes[3] = {
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptors, .dstBinding = 0, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sampled},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptors, .dstBinding = 1, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &output},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptors, .dstBinding = 2, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &reference}};
            p_vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
            VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
            VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1, .pSetLayouts = &descriptor_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
            VkPipelineLayout pipeline_layout;
            CHECK(p_vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));
            VkComputePipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = shader, .pName = "main"}, .layout = pipeline_layout};
            VkPipeline pipeline;
            CHECK(p_vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(p_vkBeginCommandBuffer(command, &begin));
            p_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            p_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptors, 1, &dynamic_offset);
            uint32_t shape[] = {width, height, LAYERS, f >= 14 ? 3 : f >= 8 && f < 12 ? 1 + (f & 1) : 0, linear_filter[f] != 0};
            p_vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shape), shape);
            VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &before, 0, NULL, 0, NULL);
            p_vkCmdFillBuffer(command, buffers[1], 0, BYTES, 0xcdcdcdcd);
            VkMemoryBarrier filled = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &filled, 0, NULL, 0, NULL);
            for (unsigned i = 0; i < 4; ++i)
                image_barrier(p_vkCmdPipelineBarrier, command, images[i], i >= 2 ? 0 : mip, i == 2 && shape_index ? 1 : LAYERS,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy reference_upload = {.bufferOffset = REFERENCE,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS}, .imageExtent = {width, height, 1}};
            p_vkCmdCopyBufferToImage(command, buffers[0], images[3], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reference_upload);
            image_barrier(p_vkCmdPipelineBarrier, command, images[3], 0, LAYERS,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            VkBufferImageCopy upload = {.bufferOffset = 16, .bufferRowLength = 40, .bufferImageHeight = 36,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, LAYERS}, .imageExtent = {width, height, 1}};
            if (use2) {
                VkBufferImageCopy2 region = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                    .bufferOffset = upload.bufferOffset, .bufferRowLength = upload.bufferRowLength,
                    .bufferImageHeight = upload.bufferImageHeight, .imageSubresource = upload.imageSubresource, .imageExtent = upload.imageExtent};
                VkCopyBufferToImageInfo2 copy = {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, .srcBuffer = buffers[0],
                    .dstImage = images[0], .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .regionCount = 1, .pRegions = &region};
                copy_to_image2(command, &copy);
            } else p_vkCmdCopyBufferToImage(command, buffers[0], images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
            int patched = width >= 8 && height >= 4;
            if (patched) {
                /* A GPU-produced block overwrites a nonzero x/layer subregion.
                 * Host upload bytes at this offset deliberately contain 0xa5. */
                p_vkCmdFillBuffer(command, buffers[0], 8192, block_bytes, bc_fill_word(mode));
                VkMemoryBarrier ready = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
                p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 1, &ready, 0, NULL, 0, NULL);
                image_barrier(p_vkCmdPipelineBarrier, command, images[0], mip, LAYERS,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkBufferImageCopy patch = {.bufferOffset = 8192, .bufferRowLength = 8, .bufferImageHeight = 8,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 1},
                    .imageOffset = {4, 0, 0}, .imageExtent = {4, 4, 1}};
                p_vkCmdCopyBufferToImage(command, buffers[0], images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &patch);
            }
            image_event(p_vkCmdSetEvent, p_vkCmdWaitEvents, set_event2, wait_events2,
                command, event, images[0], mip, LAYERS, use_sync2);
            VkImageCopy copy_region = {.srcSubresource = upload.imageSubresource,
                .dstSubresource = upload.imageSubresource, .extent = upload.imageExtent};
            if (use2) {
                VkImageCopy2 region = {.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2, .srcSubresource = copy_region.srcSubresource,
                    .dstSubresource = copy_region.dstSubresource, .extent = copy_region.extent};
                VkCopyImageInfo2 copy = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
                    .srcImage = images[0], .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .dstImage = images[1], .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .regionCount = 1, .pRegions = &region};
                copy_image2(command, &copy);
            } else p_vkCmdCopyImage(command, images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
            image_barrier(p_vkCmdPipelineBarrier, command, images[1], mip, LAYERS,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            copy_region.dstSubresource.mipLevel = 0;
            if (shape_index) { copy_region.dstSubresource.layerCount = 1; copy_region.extent.depth = LAYERS; }
            p_vkCmdCopyImage(command, images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                images[2], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
            image_barrier(p_vkCmdPipelineBarrier, command, images[2], 0, shape_index ? 1 : LAYERS,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            image_barrier(p_vkCmdPipelineBarrier, command, images[1], mip, LAYERS,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            copy_region.srcSubresource.mipLevel = 0;
            copy_region.srcSubresource.layerCount = shape_index ? 1 : LAYERS;
            copy_region.dstSubresource.mipLevel = mip;
            copy_region.dstSubresource.layerCount = LAYERS;
            /* A native texel expands to a whole 4x4 destination block. Only
             * full blocks fit; compressed edge texels were copied BC-to-BC. */
            copy_region.extent = (VkExtent3D){width / 4, height / 4, shape_index ? LAYERS : 1};
            if (copy_region.extent.width && copy_region.extent.height)
                p_vkCmdCopyImage(command, images[2], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
            if (use_sync2) {
                VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = images[1], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, LAYERS}};
                VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
                pipeline_barrier2(command, &dependency);
            } else image_barrier(p_vkCmdPipelineBarrier, command, images[1], mip, LAYERS,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            /* No application rebind after any injected decoder dispatch. */
            p_vkCmdDispatch(command, (width * height * LAYERS + 63) / 64, 1, 1);
            image_barrier(p_vkCmdPipelineBarrier, command, images[1], mip, LAYERS,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy download = {.bufferOffset = RAW, .imageSubresource = upload.imageSubresource, .imageExtent = upload.imageExtent};
            if (use2) {
                VkBufferImageCopy2 region = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, .bufferOffset = RAW,
                    .imageSubresource = download.imageSubresource, .imageExtent = download.imageExtent};
                VkCopyImageToBufferInfo2 copy = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                    .srcImage = images[1], .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .dstBuffer = buffers[1],
                    .regionCount = 1, .pRegions = &region};
                copy_to_buffer2(command, &copy);
            } else p_vkCmdCopyImageToBuffer(command, images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[1], 1, &download);
            VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
            CHECK(p_vkEndCommandBuffer(command));
            /* The interceptor must retain its own compatible layout for replay. */
            p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
            for (unsigned round = 0; round < 3; ++round) {
                struct hybris_bc_region fixture = {.width = width, .height = height, .layers = LAYERS,
                    .row_length = 40, .image_height = 36, .source_offset = 16, .source_range = BYTES};
                bc_fill_fixture(mapped[0] + binding[0], &fixture, mode, round);
                uint32_t *reference_pixels = (uint32_t *)(mapped[0] + binding[0] + REFERENCE);
                for (uint32_t pixel = 0; pixel < width * height * LAYERS; ++pixel) {
                    unsigned x = pixel % width, y = pixel / width % height, z = pixel / (width * height);
                    if (f >= 14) {
                        unsigned variant = bc_variant(mode, x / 4, y / 4, z, round);
                        unsigned at = 2 * ((y & 3) * 4 + (x & 3));
                        reference_pixels[2 * pixel] = bc6h_vectors[variant].rgba[mode - 10][at];
                        reference_pixels[2 * pixel + 1] = bc6h_vectors[variant].rgba[mode - 10][at + 1];
                        if (patched && z == 1 && x >= 4 && x < 8 && y < 4) {
                            reference_pixels[2 * pixel] = 0;
                            reference_pixels[2 * pixel + 1] = 0x3c000000;
                        }
                        continue;
                    }
                    uint32_t reference_pixel = bc_golden(mode, bc_variant(mode, x / 4, y / 4, z, round),
                        (y & 3) * 4 + (x & 3), round);
                    if (patched && z == 1 && x >= 4 && x < 8 && y < 4) reference_pixel = bc_fill_golden(mode, (y & 3) * 4 + (x & 3));
                    bc_reference_store(reference_pixels, f, pixel, reference_pixel);
                }
                VkMappedMemoryRange upload_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = buffer_memory[0], .size = VK_WHOLE_SIZE};
                CHECK(p_vkFlushMappedMemoryRanges(device, 1, &upload_range));
                VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
                CHECK(p_vkResetEvent(device, event));
                CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
                CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
                VkMappedMemoryRange read_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = buffer_memory[1], .size = VK_WHOLE_SIZE};
                CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &read_range));
                uint32_t *actual = (uint32_t *)(mapped[1] + binding[1]);
                uint32_t pixels = width * height * LAYERS;
                uint32_t columns = (width + 3) / 4, rows = (height + 3) / 4;
                uint32_t raw_size = columns * rows * LAYERS * block_bytes;
                struct bc_image_readback check = {.actual = actual, .source = mapped[0] + binding[0],
                    .region = fixture, .bytes = BYTES, .dynamic_offset = dynamic_offset,
                    .raw_offset = RAW, .reference_offset = REFERENCE, .format = f, .round = round,
                    .swizzle = view_swizzled, .patched = patched, .filtering = linear_filter[f],
                    .sampler_choice = sampler_choice};
                unsigned bad = bc_verify_image_readback(&check);
                printf("BC_IMAGES_READBACK format=%u shape=%u mip=%u round=%u copy2=%u sync2=%u gpu_patch=%u linear_filter=%u border=%u pixels=%u raw_bytes=%u bad=%u\n",
                    bc_formats[f], shape_index, mip, round, use2, use_sync2, patched, linear_filter[f] != 0, sampler_choice >= 2 ? sampler_choice - 1 : 0, pixels, raw_size, bad);
                failures += bad; ++readbacks;
                CHECK(p_vkResetFences(device, 1, &fence));
            }
            if (mip == 0) CHECK(p_vkResetCommandBuffer(command, 0));
            else if (mip == 1) CHECK(p_vkResetCommandPool(device, pool, 0));
            else if (mip == 2) {
                p_vkFreeCommandBuffers(device, pool, 1, &command);
                CHECK(p_vkAllocateCommandBuffers(device, &command_info, &command));
            }
            p_vkDestroyPipeline(device, pipeline, NULL);
            p_vkDestroyImageView(device, view, NULL);
            p_vkDestroyImageView(device, reference_view, NULL);
            for (unsigned i = 0; i < 4; ++i) {
                p_vkDestroyImage(device, images[i], NULL);
                p_vkFreeMemory(device, image_memory[i], NULL);
            }
        }
        }
    }
    p_vkDestroyEvent(device, event, NULL);
    p_vkDestroyFence(device, fence, NULL);
    p_vkDestroyCommandPool(device, pool, NULL);
    p_vkDestroyShaderModule(device, shader, NULL);
    for (unsigned i = 0; i < 5; ++i) p_vkDestroySampler(device, samplers[i], NULL);
    p_vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    p_vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    for (unsigned i = 0; i < 2; ++i) {
        p_vkUnmapMemory(device, buffer_memory[i]);
        p_vkDestroyBuffer(device, buffers[i], NULL);
        p_vkFreeMemory(device, buffer_memory[i], NULL);
    }
    p_vkDestroyDevice(device, NULL);
    if (destroy_messenger) destroy_messenger(instance, messenger, NULL);
    p_vkDestroyInstance(instance, NULL); dlclose(h);
    printf("BC_IMAGES_SUMMARY formats=16 shapes=2 readbacks=%u copy2_cases=%u sync2_cases=%u maintenance4=%u format_list=%u failures=%u validation_errors=%u route=%d\n",
        readbacks, copies2_cases, sync2_cases, maintenance4, format_list, failures, validation.errors, route);
    return failures || validation.errors || readbacks != BC_IMAGE_FORMAT_COUNT * 2 * 4 * 3 ? 2 : 0;
}
