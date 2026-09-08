/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "bc_policy.h"
#include "bc_context.h"
#include "../icd/wsi.h"
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t policy_once = PTHREAD_ONCE_INIT;
static int policy;
static void initialize_policy(void)
{
    const char *value = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_BC_TEXTURES");
    if (value && !strcmp(value, "missing")) policy = 1;
    else if (value && !strcmp(value, "force")) policy = 2;
}
int hybris_bc_enabled(void)
{
    pthread_once(&policy_once, initialize_policy);
    return policy != 0;
}
#define PHYSICAL_PROC(name) PFN_vk##name name = (PFN_vk##name)context.resolver(context.instance, "vk" #name)
unsigned hybris_bc_physical_mask(VkPhysicalDevice physical)
{
    struct hybris_icd_physical context;
    if (!hybris_bc_enabled() || !hybris_icd_lookup_physical(physical, &context) ||
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
    unsigned mask = 0;
    for (unsigned i = 0; i < 12; ++i) {
        VkFormat format = VK_FORMAT_BC1_RGB_UNORM_BLOCK + i;
        VkFormatProperties native, decoded;
        GetPhysicalDeviceFormatProperties(physical, format, &native);
        if (policy == 1 && (native.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
        GetPhysicalDeviceFormatProperties(physical, hybris_bc_image_format(format), &decoded);
        VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((decoded.optimalTilingFeatures & required) == required) mask |= 1u << i;
    }
    return mask;
}
static int emulates(VkPhysicalDevice physical, VkFormat format)
{
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC5_SNORM_BLOCK &&
        (hybris_bc_physical_mask(physical) & (1u << (format - VK_FORMAT_BC1_RGB_UNORM_BLOCK)));
}
int hybris_bc_format_properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties)
{
    if (!emulates(physical, format)) return 0;
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return 0;
    PHYSICAL_PROC(GetPhysicalDeviceFormatProperties);
    VkFormatProperties decoded;
    GetPhysicalDeviceFormatProperties(physical, hybris_bc_image_format(format), &decoded);
    *properties = (VkFormatProperties){.optimalTilingFeatures = decoded.optimalTilingFeatures &
        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)};
    return 1;
}
int hybris_bc_extension_allowed(const char *name)
{
    static const char *const unsupported[] = {
        "VK_EXT_descriptor_buffer", "VK_KHR_push_descriptor", "VK_EXT_shader_object",
        "VK_KHR_maintenance6", "VK_KHR_maintenance8", "VK_EXT_host_image_copy",
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
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV: return 1;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV: return 3;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT: return 2;
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
    if (info->pEnabledFeatures && (info->pEnabledFeatures->pipelineStatisticsQuery || info->pEnabledFeatures->textureCompressionBC)) return VK_ERROR_FEATURE_NOT_PRESENT;
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
             ((const VkPhysicalDeviceFeatures2 *)node)->features.textureCompressionBC)) return VK_ERROR_FEATURE_NOT_PRESENT;
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
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PHYSICAL_PROC(GetPhysicalDeviceProperties); PHYSICAL_PROC(GetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory;
    GetPhysicalDeviceProperties(physical, &properties);
    GetPhysicalDeviceMemoryProperties(physical, &memory);
    return hybris_bc_device_add(device, resolver, &memory, &properties, hybris_bc_physical_mask(physical), allocator);
}
static void VKAPI_CALL queue_properties(VkPhysicalDevice physical, uint32_t *count,
    VkQueueFamilyProperties *out)
{
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
    PHYSICAL_PROC(GetPhysicalDeviceQueueFamilyProperties);
    GetPhysicalDeviceQueueFamilyProperties(physical, count, out);
    if (out && hybris_bc_physical_mask(physical))
        for (uint32_t i = 0; i < *count; ++i) out[i].queueFlags &= ~VK_QUEUE_PROTECTED_BIT;
}
static void queue_properties2(VkPhysicalDevice physical, uint32_t *count,
    VkQueueFamilyProperties2 *out, const char *name)
{
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
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
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
    PHYSICAL_PROC(GetPhysicalDeviceFeatures);
    GetPhysicalDeviceFeatures(physical, out);
    if (hybris_bc_physical_mask(physical)) out->pipelineStatisticsQuery = out->textureCompressionBC = VK_FALSE;
}
static void features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *out, const char *name)
{
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)context.resolver(context.instance, name);
    query(physical, out);
    if (!hybris_bc_physical_mask(physical)) return;
    out->features.pipelineStatisticsQuery = out->features.textureCompressionBC = VK_FALSE;
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
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
    PHYSICAL_PROC(GetPhysicalDeviceSparseImageFormatProperties);
    GetPhysicalDeviceSparseImageFormatProperties(physical, format, type, samples, usage, tiling, count, out);
}
static void sparse_properties2(VkPhysicalDevice physical, const VkPhysicalDeviceSparseImageFormatInfo2 *info,
    uint32_t *count, VkSparseImageFormatProperties2 *out, const char *name)
{
    if (emulates(physical, info->format)) { *count = 0; return; }
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return;
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
static int image_supported(VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    return type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL && usage &&
        !(usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
        !(flags & ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
}
static VkResult VKAPI_CALL image_properties(VkPhysicalDevice physical, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *out)
{
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PHYSICAL_PROC(GetPhysicalDeviceImageFormatProperties);
    if (emulates(physical, format)) {
        if (!image_supported(type, tiling, usage, flags)) { *out = (VkImageFormatProperties){0}; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
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
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 query =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)context.resolver(context.instance, name);
    if (!emulates(physical, info->format)) return query(physical, info, out);
    out->imageFormatProperties = (VkImageFormatProperties){0};
    if (!image_supported(info->type, info->tiling, info->usage, info->flags)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) {
            if (((const VkPhysicalDeviceExternalImageFormatInfo *)node)->handleType) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else if (node->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *list = (const void *)node;
            for (uint32_t i = 0; i < list->viewFormatCount; ++i)
                if (list->pViewFormats[i] != info->format) return VK_ERROR_FORMAT_NOT_SUPPORTED;
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
