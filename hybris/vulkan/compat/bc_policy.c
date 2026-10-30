/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "bc_policy.h"
#include "bc_context.h"
#include "../layer/layer.h"
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <stdio.h>

static pthread_once_t policy_once = PTHREAD_ONCE_INIT;
static int policy;
static void initialize_policy(void)
{
    if (getauxval(AT_SECURE)) return;
    const char *value = getenv("HYBRIS_BC_TEXTURES");
    if (!value || !strcmp(value, "missing") || !strcmp(value, "1")) policy = 1;
    else if (value && !strcmp(value, "force")) policy = 2;
}
int hybris_bc_enabled(void)
{
    pthread_once(&policy_once, initialize_policy);
    return policy != 0;
}
#define PHYSICAL_PROC(name) PFN_vk##name name = (PFN_vk##name)context.resolver(context.instance, "vk" #name)
void hybris_bc_enable_transcode_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *features)
{
    struct hybris_layer_physical context;
    if (!hybris_bc_enabled() || !hybris_layer_lookup_physical(physical, &context)) return;
    const char *value = getauxval(AT_SECURE) ? "0" : getenv("HYBRIS_BC_TRANSCODE");
    if (value && !strcmp(value, "0")) return;
    if (!hybris_bc_physical_mask(physical)) return;
    PHYSICAL_PROC(GetPhysicalDeviceFeatures);
    VkPhysicalDeviceFeatures native;
    GetPhysicalDeviceFeatures(physical, &native);
    features->textureCompressionETC2 |= native.textureCompressionETC2;
    features->textureCompressionASTC_LDR |= native.textureCompressionASTC_LDR;
}
/* 1: core Vulkan 1.3, 2: extension required at native device creation. */
int hybris_bc_astc_hdr(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    const char *value = getauxval(AT_SECURE) ? "0" : getenv("HYBRIS_BC_TRANSCODE");
    if ((value && !strcmp(value, "0")) || !hybris_bc_physical_mask(physical) ||
        !hybris_layer_lookup_physical(physical, &context)) return 0;
    PHYSICAL_PROC(GetPhysicalDeviceProperties);
    VkPhysicalDeviceProperties properties;
    GetPhysicalDeviceProperties(physical, &properties);
    int core = context.api_version >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3;
    if (!core) {
        PHYSICAL_PROC(EnumerateDeviceExtensionProperties);
        uint32_t count = 0;
        if (EnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS) return 0;
        VkExtensionProperties *extensions = calloc(count ? count : 1, sizeof(*extensions));
        if (!extensions) return 0;
        VkResult result = EnumerateDeviceExtensionProperties(physical, NULL, &count, extensions);
        int found = 0;
        if (result == VK_SUCCESS)
            for (uint32_t i = 0; i < count; ++i)
                found |= !strcmp(extensions[i].extensionName, VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);
        free(extensions);
        if (!found) return 0;
    }
    PHYSICAL_PROC(GetPhysicalDeviceFeatures2);
    if (!GetPhysicalDeviceFeatures2) return 0;
    VkPhysicalDeviceTextureCompressionASTCHDRFeatures hdr = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &hdr};
    GetPhysicalDeviceFeatures2(physical, &features);
    return hdr.textureCompressionASTC_HDR ? (core ? 1 : 2) : 0;
}
VkFormat hybris_bc_storage_format(VkPhysicalDevice physical, const VkImageCreateInfo *info)
{
    if (!(info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (info->usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT))) return VK_FORMAT_UNDEFINED;
    VkPhysicalDeviceFeatures enabled = {0};
    hybris_bc_enable_transcode_features(physical, &enabled);
    VkFormat target = hybris_bc_transcode_format(info->format);
    if (target == VK_FORMAT_UNDEFINED) return target;
    if (target == VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK) {
        if (!hybris_bc_astc_hdr(physical)) return VK_FORMAT_UNDEFINED;
    } else if (target == VK_FORMAT_ASTC_4x4_UNORM_BLOCK || target == VK_FORMAT_ASTC_4x4_SRGB_BLOCK) {
        if (!enabled.textureCompressionASTC_LDR) return VK_FORMAT_UNDEFINED;
    } else if (!enabled.textureCompressionETC2) return VK_FORMAT_UNDEFINED;
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return VK_FORMAT_UNDEFINED;
    PHYSICAL_PROC(GetPhysicalDeviceImageFormatProperties); PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
    VkFormatProperties formats;
    GetPhysicalDeviceFormatProperties(physical, target, &formats);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((formats.optimalTilingFeatures & need) != need) return VK_FORMAT_UNDEFINED;
    VkImageFormatProperties limits;
    if (GetPhysicalDeviceImageFormatProperties(physical, target, info->imageType, info->tiling,
        info->usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, info->flags, &limits) != VK_SUCCESS ||
        info->extent.width > limits.maxExtent.width || info->extent.height > limits.maxExtent.height ||
        info->extent.depth > limits.maxExtent.depth || info->mipLevels > limits.maxMipLevels ||
        info->arrayLayers > limits.maxArrayLayers || !(limits.sampleCounts & info->samples)) return VK_FORMAT_UNDEFINED;
    return target;
}
/* MaliBridge's BC1 UNORM fallback: use 16-bit images only when the native
 * format supports sampling, filtering, copies and blits. */
static unsigned physical_packed_mask(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return 0;
    PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
    const VkFormat formats[2] = {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16};
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    unsigned mask = 0;
    for (unsigned i = 0; i < 2; ++i) {
        VkFormatProperties properties;
        GetPhysicalDeviceFormatProperties(physical, formats[i], &properties);
        if ((properties.optimalTilingFeatures & required) == required) mask |= 1u << i;
    }
    return mask;
}
unsigned hybris_bc_physical_mask(VkPhysicalDevice physical)
{
    struct hybris_layer_physical context;
    if (!hybris_bc_enabled() || !hybris_layer_lookup_physical(physical, &context) ||
        context.api_version < VK_API_VERSION_1_1) return 0;
    PHYSICAL_PROC(GetPhysicalDeviceProperties); PHYSICAL_PROC(GetPhysicalDeviceQueueFamilyProperties);
    PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
    VkPhysicalDeviceProperties properties;
    GetPhysicalDeviceProperties(physical, &properties);
    /* Core 1.4 includes descriptor-state commands that are not intercepted yet. */
    if (properties.apiVersion < VK_API_VERSION_1_1 || properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0) ||
        properties.limits.maxStorageBufferRange < 256 ||
        properties.limits.minStorageBufferOffsetAlignment >= properties.limits.maxStorageBufferRange) return 0;
    uint32_t count = 0;
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties queues[32];
    if (!count || count > sizeof(queues) / sizeof(queues[0])) return 0;
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    for (uint32_t i = 0; i < count; ++i) {
        if (!(queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) continue;
        /* Every queue allowed to copy these formats must execute the decoder.
         * Four-row decode tiles need single-texel native transfer granularity. */
        if (!(queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
            queues[i].minImageTransferGranularity.width != 1 ||
            queues[i].minImageTransferGranularity.height != 1 ||
            queues[i].minImageTransferGranularity.depth != 1) return 0;
    }
    PHYSICAL_PROC(GetPhysicalDeviceFeatures);
    VkPhysicalDeviceFeatures features;
    GetPhysicalDeviceFeatures(physical, &features);
    if ((policy == 1 && features.textureCompressionBC) || !features.shaderStorageImageExtendedFormats)
        return 0;
    const VkFormat storage[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM,
        VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (unsigned i = 0; i < sizeof(storage) / sizeof(storage[0]); ++i) {
        VkFormatProperties format;
        GetPhysicalDeviceFormatProperties(physical, storage[i], &format);
        if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
    }
    /* Admit the complete decoder family, as MaliBridge does. Format/image
     * queries still return the native backing's actual per-usage limits. */
    for (unsigned i = 0; i < 16; ++i) {
        VkFormatProperties decoded;
        GetPhysicalDeviceFormatProperties(physical,
            hybris_bc_image_format(VK_FORMAT_BC1_RGB_UNORM_BLOCK + i), &decoded);
        VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((decoded.optimalTilingFeatures & required) != required) return 0;
    }
    return 0xffffu;
}
static int emulates(VkPhysicalDevice physical, VkFormat format)
{
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK &&
        (hybris_bc_physical_mask(physical) & (1u << (format - VK_FORMAT_BC1_RGB_UNORM_BLOCK)));
}
/* MaliBridge exposes the BC family once its decoded representations work;
 * optional image flags and dimensional limits are queried per image instead of
 * vetoing every BC texture when one optional usage is unavailable. */
int hybris_bc_complete(VkPhysicalDevice physical)
{
    return hybris_bc_physical_mask(physical) != 0;
}
int hybris_bc_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties)
{
    if (!emulates(physical, format)) return 0;
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return 0;
    PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
    VkFormatProperties decoded;
    GetPhysicalDeviceFormatProperties(physical, hybris_bc_image_format(format), &decoded);
    *properties = (VkFormatProperties){.optimalTilingFeatures = decoded.optimalTilingFeatures &
        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
         VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT)};
    /* The public image handle is the decoded native image. Source blits can
     * use the driver's existing conversion/filtering path without a new
     * command wrapper. Destination blits remain unsupported: they would need
     * to update the compressed backing as well as the decoded image. */
    return 1;
}
int hybris_bc_extension_allowed(const char *name)
{
    static const char *const unsupported[] = {
        "VK_EXT_descriptor_buffer", "VK_KHR_push_descriptor", "VK_EXT_shader_object",
        "VK_NV_per_stage_descriptor_set", "VK_KHR_maintenance8", "VK_EXT_host_image_copy",
        "VK_QCOM_rotated_copy_commands", "VK_EXT_device_generated_commands",
        "VK_NV_device_generated_commands", "VK_NV_device_generated_commands_compute",
        "VK_NV_dedicated_allocation", "VK_NV_copy_memory_indirect"
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
        if (!strcmp(name, unsupported[i])) return 0;
    return 1;
}
/* Extension feature queries must agree with the filtered extension list. Each
 * structure has the usual sType/pNext prefix followed by these VkBool32s. */
static unsigned unsupported_feature_count(VkStructureType type)
{
    switch (type) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT: return 4;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV: return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV: return 3;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT: return 2;
    /* Per-stage descriptor binding and inline pipeline layouts cannot be
     * restored by the compute-state journal. maintenance6 alone needs neither. */
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV: return 2;
#ifdef VK_KHR_maintenance8
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR: return 1;
#endif
    default: return 0;
    }
}
VkResult hybris_bc_prepare_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info)
{
    if (!hybris_bc_physical_mask(physical)) return VK_SUCCESS;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (!hybris_bc_extension_allowed(info->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
    int complete = hybris_bc_complete(physical);
    if (info->pEnabledFeatures && (info->pEnabledFeatures->pipelineStatisticsQuery ||
        (info->pEnabledFeatures->textureCompressionBC && !complete))) return VK_ERROR_FEATURE_NOT_PRESENT;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        unsigned count = unsupported_feature_count(node->sType);
        for (unsigned i = 0; i < count; ++i) {
            VkBool32 enabled;
            memcpy(&enabled, (const char *)node + sizeof(VkBaseInStructure) + i * sizeof(enabled), sizeof(enabled));
            if (enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
            ((const VkDeviceGroupDeviceCreateInfo *)node)->physicalDeviceCount != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 &&
            (((const VkPhysicalDeviceFeatures2 *)node)->features.pipelineStatisticsQuery ||
             (((const VkPhysicalDeviceFeatures2 *)node)->features.textureCompressionBC && !complete))) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES &&
            ((const VkPhysicalDeviceVulkan11Features *)node)->protectedMemory) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES &&
            ((const VkPhysicalDeviceProtectedMemoryFeatures *)node)->protectedMemory) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return VK_SUCCESS;
}
VkResult hybris_bc_attach_device(VkDevice device, VkPhysicalDevice physical,
    PFN_vkGetDeviceProcAddr resolver, const VkAllocationCallbacks *allocator)
{
    if (!hybris_bc_enabled()) return VK_SUCCESS;
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PHYSICAL_PROC(GetPhysicalDeviceProperties); PHYSICAL_PROC(GetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    GetPhysicalDeviceProperties(physical, &properties);
    GetPhysicalDeviceMemoryProperties(physical, &memory);
    VkResult result = hybris_bc_device_add(device, resolver, &memory, &properties,
        hybris_bc_physical_mask(physical), physical_packed_mask(physical), allocator);
    if (result == VK_SUCCESS) hybris_bc_device_find(device)->physical = physical;
    return result;
}
static void VKAPI_CALL queue_properties(VkPhysicalDevice physical, uint32_t *count,
    VkQueueFamilyProperties *out)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PHYSICAL_PROC(GetPhysicalDeviceQueueFamilyProperties);
    GetPhysicalDeviceQueueFamilyProperties(physical, count, out);
    if (out && hybris_bc_physical_mask(physical))
        for (uint32_t i = 0; i < *count; ++i) out[i].queueFlags &= ~VK_QUEUE_PROTECTED_BIT;
}
static void queue_properties2(VkPhysicalDevice physical, uint32_t *count,
    VkQueueFamilyProperties2 *out, const char *name)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 query =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)context.resolver(context.instance, name);
    query(physical, count, out);
    if (out && hybris_bc_physical_mask(physical))
        for (uint32_t i = 0; i < *count; ++i) out[i].queueFamilyProperties.queueFlags &= ~VK_QUEUE_PROTECTED_BIT;
}
static void VKAPI_CALL queue_properties2_core(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties2 *out)
{ queue_properties2(physical, count, out, "vkGetPhysicalDeviceQueueFamilyProperties2"); }
static void VKAPI_CALL queue_properties2_khr(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties2 *out)
{ queue_properties2(physical, count, out, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"); }
static void VKAPI_CALL features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures *out)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PHYSICAL_PROC(GetPhysicalDeviceFeatures);
    GetPhysicalDeviceFeatures(physical, out);
    if (hybris_bc_physical_mask(physical)) {
        out->pipelineStatisticsQuery = VK_FALSE;
        out->textureCompressionBC = hybris_bc_complete(physical);
    }
}
static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out, const char *name)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)context.resolver(context.instance, name);
    query(physical, out);
    if (!hybris_bc_physical_mask(physical)) return;
    out->features.pipelineStatisticsQuery = VK_FALSE;
    out->features.textureCompressionBC = hybris_bc_complete(physical);
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext) {
        unsigned count = unsupported_feature_count(node->sType);
        if (count) memset((char *)node + sizeof(VkBaseOutStructure), 0, count * sizeof(VkBool32));
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            ((VkPhysicalDeviceVulkan11Features *)node)->protectedMemory = VK_FALSE;
        else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES)
            ((VkPhysicalDeviceProtectedMemoryFeatures *)node)->protectedMemory = VK_FALSE;
    }
}
static void VKAPI_CALL features2_core(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2"); }
static void VKAPI_CALL features2_khr(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out)
{ features2(physical, out, "vkGetPhysicalDeviceFeatures2KHR"); }
static void VKAPI_CALL sparse_properties(VkPhysicalDevice physical, VkFormat format,
    VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
    VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *out)
{
    if (emulates(physical, format)) { *count = 0; return; }
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PHYSICAL_PROC(GetPhysicalDeviceSparseImageFormatProperties);
    GetPhysicalDeviceSparseImageFormatProperties(physical, format, type, samples, usage, tiling, count, out);
}
static void sparse_properties2(VkPhysicalDevice physical, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *out, const char *name)
{
    if (emulates(physical, info->format)) { *count = 0; return; }
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2 query =
        (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2)context.resolver(context.instance, name);
    query(physical, info, count, out);
}
static void VKAPI_CALL sparse_properties2_core(VkPhysicalDevice physical, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *out)
{ sparse_properties2(physical, info, count, out, "vkGetPhysicalDeviceSparseImageFormatProperties2"); }
static void VKAPI_CALL sparse_properties2_khr(VkPhysicalDevice physical, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *out)
{ sparse_properties2(physical, info, count, out, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR"); }
static int image_supported(VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    return (type == VK_IMAGE_TYPE_2D || type == VK_IMAGE_TYPE_3D) &&
        (type != VK_IMAGE_TYPE_3D || !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) &&
        tiling == VK_IMAGE_TILING_OPTIMAL && usage &&
        !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
        !(flags & ~(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                    VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT |
                    VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT | VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT)) &&
        (!(flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || hybris_bc_mutable_format(format));
}
static void trace_image_query(VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, const void *chain)
{
    const char *trace = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_TRACE");
    if (!trace || strcmp(trace, "1")) return;
    fprintf(stderr, "HYBRIS_BC_QUERY format=%u type=%u tiling=%u usage=0x%x flags=0x%x",
        format, type, tiling, usage, flags);
    for (const VkBaseInStructure *node = chain; node; node = node->pNext)
        fprintf(stderr, " next=%u", node->sType);
    fprintf(stderr, "\n");
}
static VkResult VKAPI_CALL image_properties(VkPhysicalDevice physical, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *out)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PHYSICAL_PROC(GetPhysicalDeviceImageFormatProperties);
    if (emulates(physical, format)) {
        trace_image_query(format, type, tiling, usage, flags, NULL);
        if (!image_supported(format, type, tiling, usage, flags)) { *out = (VkImageFormatProperties){0}; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
        VkResult result = GetPhysicalDeviceImageFormatProperties(physical, hybris_bc_image_format(format), type,
            tiling, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, flags, out);
        if (result == VK_SUCCESS) out->sampleCounts &= VK_SAMPLE_COUNT_1_BIT;
        return result;
    }
    return GetPhysicalDeviceImageFormatProperties(physical, format, type, tiling, usage, flags, out);
}
static VkResult image_properties2(VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *info,
    VkImageFormatProperties2 *out, const char *name)
{
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 query =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)context.resolver(context.instance, name);
    if (!emulates(physical, info->format)) return query(physical, info, out);
    trace_image_query(info->format, info->type, info->tiling, info->usage, info->flags, info->pNext);
    out->imageFormatProperties = (VkImageFormatProperties){0};
    if (!image_supported(info->format, info->type, info->tiling, info->usage, info->flags)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) {
            if (((const VkPhysicalDeviceExternalImageFormatInfo *)node)->handleType) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else if (node->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *list = (const void *)node;
            for (uint32_t i = 0; i < list->viewFormatCount; ++i)
                if (!hybris_bc_view_format_allowed(info->format, list->pViewFormats[i], info->flags)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else if (node->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }
    VkPhysicalDeviceImageFormatInfo2 translated = *info;
    translated.pNext = NULL;
    translated.format = hybris_bc_image_format(info->format);
    translated.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult result = query(physical, &translated, out);
    if (result != VK_SUCCESS) return result;
    out->imageFormatProperties.sampleCounts &= VK_SAMPLE_COUNT_1_BIT;
    for (VkBaseOutStructure *node = out->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
            ((VkExternalImageFormatProperties *)node)->externalMemoryProperties = (VkExternalMemoryProperties){0};
        else if (node->sType == VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT) {
            VkFilterCubicImageViewImageFormatPropertiesEXT *cubic = (void *)node;
            cubic->filterCubic = cubic->filterCubicMinmax = VK_FALSE;
        }
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL image_properties2_core(VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{ return image_properties2(physical, info, out, "vkGetPhysicalDeviceImageFormatProperties2"); }
static VkResult VKAPI_CALL image_properties2_khr(VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *out)
{ return image_properties2(physical, info, out, "vkGetPhysicalDeviceImageFormatProperties2KHR"); }
PFN_vkVoidFunction hybris_bc_policy_proc(const char *name)
{
    if (!hybris_bc_enabled()) return NULL;
    static const struct { const char *name; PFN_vkVoidFunction function; } commands[] = {
#define ENTRY(name, function) {"vk" #name, (PFN_vkVoidFunction)function}
        ENTRY(GetPhysicalDeviceQueueFamilyProperties, queue_properties),
        ENTRY(GetPhysicalDeviceQueueFamilyProperties2, queue_properties2_core),
        ENTRY(GetPhysicalDeviceQueueFamilyProperties2KHR, queue_properties2_khr),
        ENTRY(GetPhysicalDeviceSparseImageFormatProperties, sparse_properties),
        ENTRY(GetPhysicalDeviceSparseImageFormatProperties2, sparse_properties2_core),
        ENTRY(GetPhysicalDeviceSparseImageFormatProperties2KHR, sparse_properties2_khr),
        ENTRY(GetPhysicalDeviceFeatures, features), ENTRY(GetPhysicalDeviceFeatures2, features2_core),
        ENTRY(GetPhysicalDeviceFeatures2KHR, features2_khr), ENTRY(GetPhysicalDeviceImageFormatProperties, image_properties),
        ENTRY(GetPhysicalDeviceImageFormatProperties2, image_properties2_core),
        ENTRY(GetPhysicalDeviceImageFormatProperties2KHR, image_properties2_khr)
#undef ENTRY
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(name, commands[i].name)) return commands[i].function;
    return NULL;
}
PFN_vkVoidFunction hybris_bc_proc(const char *name)
{
    if (!hybris_bc_enabled()) return NULL;
    PFN_vkVoidFunction function = hybris_bc_commands_proc(name);
    if (!function) function = hybris_bc_resources_proc(name);
    if (!function) function = hybris_bc_record_proc(name);
    if (!function) function = hybris_bc_barriers_proc(name);
    if (!function) function = hybris_bc_image_copy_proc(name);
    return function;
}
