#include "probe.h"
#include "bc_fixture.h"

/* Runs the production kernel explicitly, without claiming the ICD has BC image
 * interception. Upload has TRANSFER usage only; decoding reads a GPU copy made
 * at execution time. All mapped addresses include nonzero memory binding offsets. */
int bc_decode_probe(int validate)
{
    void *h = dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 2;
    PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
    VkInstance instance = VK_NULL_HANDLE;
    V(vkCreateInstance);
    struct validation_state validation = {0};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation_message, .pUserData = &validation};
    VkValidationFeatureEnableEXT enabled = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &debug,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &enabled};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .pNext = validate ? &vf : NULL, .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = validate ? 2 : 0, .ppEnabledExtensionNames = extensions};
    CHECK(p_vkCreateInstance(&ci, NULL, &instance));
    V(vkDestroyInstance); V(vkEnumeratePhysicalDevices); V(vkGetPhysicalDeviceProperties);
    V(vkGetPhysicalDeviceMemoryProperties); V(vkGetPhysicalDeviceQueueFamilyProperties);
    V(vkCreateDevice); V(vkDestroyDevice); V(vkGetDeviceQueue); V(vkGetDeviceProcAddr);
    V(vkCreateBuffer); V(vkDestroyBuffer); V(vkGetBufferMemoryRequirements);
    V(vkAllocateMemory); V(vkFreeMemory); V(vkBindBufferMemory);
    V(vkMapMemory); V(vkUnmapMemory); V(vkFlushMappedMemoryRanges); V(vkInvalidateMappedMemoryRanges);
    V(vkCreateDescriptorPool); V(vkDestroyDescriptorPool); V(vkAllocateDescriptorSets); V(vkUpdateDescriptorSets);
    V(vkCreateCommandPool); V(vkDestroyCommandPool); V(vkAllocateCommandBuffers);
    V(vkBeginCommandBuffer); V(vkEndCommandBuffer); V(vkCmdCopyBuffer); V(vkCmdFillBuffer); V(vkCmdPipelineBarrier);
    V(vkCreateFence); V(vkDestroyFence); V(vkWaitForFences); V(vkResetFences); V(vkQueueSubmit);
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult result = p_vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count) return 2;
    uint32_t families = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties *queues = calloc(families, sizeof(*queues));
    if (!queues) return 2;
    p_vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues);
    uint32_t family = 0;
    while (family < families && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(queues);
    if (family == families) { printf("UNSUPPORTED bc-decode no compute queue\n"); return 3; }
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceProperties(physical, &properties);
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
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
    VkQueue queue;
    p_vkGetDeviceQueue(device, family, 0, &queue);
    struct hybris_bc_decoder decoder;
    CHECK(hybris_bc_decoder_create(device, p_vkGetDeviceProcAddr, NULL, &decoder));
    enum { BYTES = 131072 };
    VkBuffer buffers[4];
    VkDeviceMemory memory[4];
    VkDeviceSize binding[4];
    uint8_t *mapped[4] = {0};
    for (unsigned i = 0; i < 4; ++i) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                ((i == 1 || i == 2) ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(p_vkCreateBuffer(device, &bi, NULL, &buffers[i]));
        VkMemoryRequirements req;
        p_vkGetBufferMemoryRequirements(device, buffers[i], &req);
        int host = i == 0 || i == 3;
        int type = find_mem(&mp, req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0);
        if (type < 0) { printf("UNSUPPORTED bc-decode memory buffer=%u\n", i); return 3; }
        binding[i] = req.alignment;
        if (binding[i] > SIZE_MAX - req.size) return 2;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = binding[i] + req.size, .memoryTypeIndex = (uint32_t)type};
        CHECK(p_vkAllocateMemory(device, &ai, NULL, &memory[i]));
        CHECK(p_vkBindBufferMemory(device, buffers[i], memory[i], binding[i]));
        if (host) CHECK(p_vkMapMemory(device, memory[i], 0, VK_WHOLE_SIZE, 0, (void **)&mapped[i]));
    }
    VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool descriptor_pool;
    CHECK(p_vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool));
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &decoder.descriptor_layout};
    VkDescriptorSet descriptors;
    CHECK(p_vkAllocateDescriptorSets(device, &set_info, &descriptors));
    VkDescriptorBufferInfo buffer_info[2] = {{buffers[1], 0, BYTES}, {buffers[2], 0, BYTES}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; ++i)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptors, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer_info[i]};
    p_vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkCommandPoolCreateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(p_vkCreateCommandPool(device, &command_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(p_vkAllocateCommandBuffers(device, &command_allocation, &command));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(p_vkCreateFence(device, &fi, NULL, &fence));
    unsigned failures = 0, readbacks = 0;
    for (unsigned variant = 0; variant < BC_FORMAT_COUNT + 4; ++variant) {
        unsigned format = variant < BC_FORMAT_COUNT ? variant : variant < BC_FORMAT_COUNT + 2 ? variant - BC_FORMAT_COUNT : 14 + variant - BC_FORMAT_COUNT - 2;
        unsigned rgb16 = variant >= BC_FORMAT_COUNT + 2;
        unsigned rgb8 = variant >= BC_FORMAT_COUNT && !rgb16;
        unsigned mode = bc_mode(format);
        for (unsigned shape = 0; shape < (mode >= 9 ? 5u : 4u); ++shape) {
            const uint32_t shapes[5][5] = {{4, 4, 1, 0, 0}, {9, 7, 3, 16, 12},
                                         {1, 1, 2, 0, 0}, {129, 5, 2, 144, 12}, {128, 64, 1, 0, 0}};
            struct hybris_bc_region region = {.format = bc_formats[format], .rgb8 = rgb8, .rgb16 = rgb16,
                .width = shapes[shape][0], .height = shapes[shape][1], .layers = shapes[shape][2],
                .row_length = shapes[shape][3], .image_height = shapes[shape][4],
                .source_offset = 12, .destination_offset = 20, .source_range = BYTES, .destination_range = BYTES};
            if (shape == 4 && mode >= 10) region.height = 120;
            VkPhysicalDeviceLimits limits = properties.limits;
            /* Exercise splitting with modest buffers, without overstating the
             * amount of device memory or actual workgroup-limit coverage. */
            if (shape == 3) limits.maxComputeWorkGroupCount[0] = 1;
            for (unsigned round = 0; round < 4; ++round) {
                int gpu_write = round == 3;
                if (round == 0 || gpu_write) {
                    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    CHECK(p_vkBeginCommandBuffer(command, &begin));
                    VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
                    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 1, &before, 0, NULL, 0, NULL);
                    if (gpu_write) {
                        p_vkCmdFillBuffer(command, buffers[0], 0, BYTES, bc_fill_word(mode));
                        VkMemoryBarrier filled = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
                        p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 1, &filled, 0, NULL, 0, NULL);
                    }
                    VkBufferCopy copy = {.size = BYTES};
                    p_vkCmdCopyBuffer(command, buffers[0], buffers[1], 1, &copy);
                    p_vkCmdFillBuffer(command, buffers[2], 0, BYTES, 0xcdcdcdcd);
                    VkMemoryBarrier uploaded = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
                    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 1, &uploaded, 0, NULL, 0, NULL);
                    if (variant == 0 && shape == 0 && round == 0) {
                        /* Rejections happen in the live recording, before any
                         * dispatch or state change; the normal readback below
                         * also checks the surrounding sentinel words. */
                        for (unsigned rejection = 0; rejection < 10; ++rejection) {
                            struct hybris_bc_region invalid = region;
                            VkPhysicalDeviceLimits invalid_limits = limits;
                            VkResult wanted = VK_ERROR_INITIALIZATION_FAILED;
                            switch (rejection) {
                            case 0: invalid.format = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK; wanted = VK_ERROR_FORMAT_NOT_SUPPORTED; break;
                            case 1: invalid.format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK; wanted = VK_ERROR_FORMAT_NOT_SUPPORTED; break;
                            case 2: invalid.source_range = invalid.source_offset + 7; break;
                            case 3: invalid.destination_range = invalid.destination_offset + 63; break;
                            case 4: invalid.source_offset = UINT64_MAX; break;
                            case 5: invalid.width = invalid.height = UINT32_MAX; break;
                            case 6: invalid.row_length = 3; break;
                            case 7: invalid.image_height = 3; break;
                            case 8: invalid_limits.maxComputeWorkGroupCount[0] = 0; break;
                            case 9: invalid_limits.maxStorageBufferRange = 1; break;
                            }
                            VkResult rejected = hybris_bc_decode_record(&decoder, command, descriptors, &invalid, &invalid_limits);
                            printf("BC_REJECT case=%u result=%d expected=%d\n", rejection, rejected, wanted);
                            if (rejected != wanted) return 2;
                        }
                    }
                    if (variant == BC_FORMAT_COUNT && shape == 0 && round == 0) {
                        struct hybris_bc_region invalid = region;
                        invalid.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                        VkResult rejected = hybris_bc_decode_record(&decoder, command, descriptors, &invalid, &limits);
                        printf("BC_REJECT_RGB8_FORMAT result=%d expected=%d\n", rejected, VK_ERROR_INITIALIZATION_FAILED);
                        if (rejected != VK_ERROR_INITIALIZATION_FAILED) return 2;
                    }
                    if (variant == BC_FORMAT_COUNT + 2 && shape == 0 && round == 0) {
                        for (unsigned rejection = 0; rejection < 3; ++rejection) {
                            struct hybris_bc_region invalid = region;
                            if (rejection == 0) invalid.format = VK_FORMAT_BC7_UNORM_BLOCK;
                            else {
                                invalid.rgb16 = rejection == 1;
                                invalid.destination_range = invalid.destination_offset + (invalid.rgb16 ? 95 : 127);
                            }
                            VkResult rejected = hybris_bc_decode_record(&decoder, command, descriptors, &invalid, &limits);
                            printf("BC_REJECT_HALF case=%u result=%d expected=%d\n", rejection, rejected, VK_ERROR_INITIALIZATION_FAILED);
                            if (rejected != VK_ERROR_INITIALIZATION_FAILED) return 2;
                        }
                    }
                    CHECK(hybris_bc_decode_record(&decoder, command, descriptors, &region, &limits));
                    VkMemoryBarrier decoded = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
                    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &decoded, 0, NULL, 0, NULL);
                    p_vkCmdCopyBuffer(command, buffers[2], buffers[3], 1, &copy);
                    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
                    p_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        0, 1, &host, 0, NULL, 0, NULL);
                    CHECK(p_vkEndCommandBuffer(command));
                }
                /* Intentionally after recording, and changed between submits. */
                bc_fill_fixture(mapped[0] + binding[0], &region, mode, round);
                VkMappedMemoryRange upload_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = memory[0], .size = VK_WHOLE_SIZE};
                CHECK(p_vkFlushMappedMemoryRanges(device, 1, &upload_range));
                VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
                CHECK(p_vkQueueSubmit(queue, 1, &submit, fence));
                CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
                VkMappedMemoryRange read_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = memory[3], .size = VK_WHOLE_SIZE};
                CHECK(p_vkInvalidateMappedMemoryRanges(device, 1, &read_range));
                uint32_t *actual = (uint32_t *)(mapped[3] + binding[3]);
                unsigned pixels = region.width * region.height * region.layers, bad = 0;
                for (unsigned word = 0; word < BYTES / 4; ++word) {
                    uint32_t expected = 0xcdcdcdcd;
                    unsigned packed = mode == 4 || mode == 5;
                    unsigned words = rgb16 ? (pixels * 6 + 3) / 4 : rgb8 ? (pixels * 3 + 3) / 4 : packed ? (pixels + 1) / 2 : pixels * (mode >= 10 ? 2 : 1);
                    if (word >= 5 && word < 5 + words) {
                        expected = 0;
                        if (rgb16) {
                            for (unsigned half = 0; half < 2; ++half) {
                                unsigned index = (word - 5) * 2 + half, pixel = index / 3, component = index % 3;
                                if (pixel >= pixels) break;
                                unsigned x = pixel % region.width, y = pixel / region.width % region.height;
                                unsigned z = pixel / (region.width * region.height);
                                unsigned variant = bc_variant(mode, x / 4, y / 4, z, round);
                                uint32_t rgba = gpu_write ? 0 : bc6h_vectors[variant].rgba[mode - 10][2 * ((y & 3) * 4 + (x & 3)) + component / 2];
                                expected |= ((rgba >> (16 * (component & 1))) & 65535u) << (16 * half);
                            }
                        } else if (mode >= 10) {
                            unsigned pixel = (word - 5) / 2, part = (word - 5) & 1;
                            unsigned x = pixel % region.width, y = pixel / region.width % region.height;
                            unsigned z = pixel / (region.width * region.height);
                            unsigned variant = bc_variant(mode, x / 4, y / 4, z, round);
                            expected = gpu_write ? (part ? 0x3c000000u : 0u) :
                                bc6h_vectors[variant].rgba[mode - 10][2 * ((y & 3) * 4 + (x & 3)) + part];
                        } else if (rgb8) {
                            for (unsigned part = 0; part < 4; ++part) {
                                unsigned byte = (word - 5) * 4 + part, pixel = byte / 3;
                                if (pixel >= pixels) break;
                                unsigned x = pixel % region.width, y = pixel / region.width % region.height;
                                unsigned z = pixel / (region.width * region.height);
                                uint32_t value = gpu_write ? bc_fill_golden(mode, (y & 3) * 4 + (x & 3)) : bc_golden(mode,
                                    bc_variant(mode, x / 4, y / 4, z, round), (y & 3) * 4 + (x & 3), round);
                                expected |= ((value >> (8 * (byte % 3))) & 255) << (8 * part);
                            }
                        } else for (unsigned part = 0; part < (packed ? 2 : 1); ++part) {
                            unsigned pixel = (word - 5) * (packed ? 2 : 1) + part;
                            if (pixel >= pixels) break;
                            unsigned x = pixel % region.width;
                            unsigned y = pixel / region.width % region.height;
                            unsigned z = pixel / (region.width * region.height);
                            uint32_t value = gpu_write ? bc_fill_golden(mode, (y & 3) * 4 + (x & 3)) :
                                bc_golden(mode, bc_variant(mode, x / 4, y / 4, z, round), (y & 3) * 4 + (x & 3), round);
                            expected |= value << (16 * part);
                        }
                    }
                    if (actual[word] != expected) {
                        if (bad < 2) printf("BC_MISMATCH word=%u actual=%08x expected=%08x\n", word, actual[word], expected);
                        ++bad;
                    }
                }
                printf("BC_DECODE format=%u rgb8=%u rgb16=%u shape=%u round=%u gpu_write=%d pixels=%u bad=%u\n",
                    bc_formats[format], rgb8, rgb16, shape, round, gpu_write, pixels, bad);
                failures += bad;
                ++readbacks;
                CHECK(p_vkResetFences(device, 1, &fence));
            }
        }
    }
    p_vkDestroyFence(device, fence, NULL);
    p_vkDestroyCommandPool(device, pool, NULL);
    p_vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    for (unsigned i = 0; i < 4; ++i) {
        if (mapped[i]) p_vkUnmapMemory(device, memory[i]);
        p_vkDestroyBuffer(device, buffers[i], NULL);
        p_vkFreeMemory(device, memory[i], NULL);
    }
    hybris_bc_decoder_destroy(&decoder, NULL);
    p_vkDestroyDevice(device, NULL);
    if (destroy_messenger) destroy_messenger(instance, messenger, NULL);
    p_vkDestroyInstance(instance, NULL);
    dlclose(h);
    printf("BC_DECODE_SUMMARY formats=16 encodings=20 bc7_corpus_blocks=512 bc6h_corpus_blocks=952 readbacks=%u failures=%u validation_errors=%u image_interception=0\n",
        readbacks, failures, validation.errors);
    return failures || validation.errors || readbacks != ((BC_FORMAT_COUNT + 4) * 4 + 6) * 4 ? 2 : 0;
}
