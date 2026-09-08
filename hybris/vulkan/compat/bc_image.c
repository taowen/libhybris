/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_image.h"
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)image->resolver(image->device, "vk" #name)

VkFormat hybris_bc_image_format(VkFormat format, unsigned rgb8_mask)
{
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        return rgb8_mask & 1 ? VK_FORMAT_R8G8B8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        return rgb8_mask & 2 ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK:
        return VK_FORMAT_R16_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK:
        return VK_FORMAT_R16G16_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK:
        return VK_FORMAT_R16G16_SNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkExtent3D hybris_bc_image_extent(const struct hybris_bc_image *image, uint32_t mip)
{
    VkExtent3D extent = {0};
    if (mip >= image->mip_levels) return extent;
    extent.width = image->extent.width >> mip;
    extent.height = image->extent.height >> mip;
    if (!extent.width) extent.width = 1;
    if (!extent.height) extent.height = 1;
    extent.depth = 1;
    return extent;
}

VkDeviceSize hybris_bc_image_layer_size(const struct hybris_bc_image *image, uint32_t mip)
{
    VkExtent3D extent = hybris_bc_image_extent(image, mip);
    return ((extent.width + UINT64_C(3)) / 4) * ((extent.height + UINT64_C(3)) / 4) * image->block_bytes;
}

void hybris_bc_image_destroy(struct hybris_bc_image *image, const VkAllocationCallbacks *allocator)
{
    if (!image || !image->resolver) return;
    PROC(DestroyImage); PROC(DestroyBuffer); PROC(FreeMemory);
    if (image->image) DestroyImage(image->device, image->image, allocator);
    if (image->blocks) DestroyBuffer(image->device, image->blocks, allocator);
    if (image->decoded_memory) FreeMemory(image->device, image->decoded_memory, NULL);
    memset(image, 0, sizeof(*image));
}

VkResult hybris_bc_image_describe(const VkImageCreateInfo *info, struct hybris_bc_image *image)
{
    if (!info || !image) return VK_ERROR_INITIALIZATION_FAILED;
    memset(image, 0, sizeof(*image));
    VkFormat format = hybris_bc_image_format(info->format, 0);
    if (format == VK_FORMAT_UNDEFINED ||
        info->imageType != VK_IMAGE_TYPE_2D || info->tiling != VK_IMAGE_TILING_OPTIMAL ||
        info->samples != VK_SAMPLE_COUNT_1_BIT ||
        (info->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED && info->initialLayout != VK_IMAGE_LAYOUT_PREINITIALIZED) ||
        (info->flags & ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
        !info->usage ||
        (info->usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ||
        !info->extent.width || !info->extent.height || info->extent.depth != 1 ||
        !info->mipLevels || info->mipLevels > 32 || !info->arrayLayers)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    /* These chains only constrain views or request no external/swapchain
     * binding. The decoded image has the same view restriction in our wrapper. */
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *list = (const void *)next;
            for (uint32_t i = 0; i < list->viewFormatCount; ++i)
                if (list->pViewFormats[i] != info->format) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else if (next->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
            if (((const VkExternalMemoryImageCreateInfo *)next)->handleTypes)
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else if (next->sType == VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR) {
            if (((const VkImageSwapchainCreateInfoKHR *)next)->swapchain)
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
        } else {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }
    uint32_t dimension = info->extent.width > info->extent.height ? info->extent.width : info->extent.height;
    uint32_t levels = 1;
    while (dimension >>= 1) ++levels;
    if (info->mipLevels > levels) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    image->format = info->format;
    image->extent = info->extent;
    image->mip_levels = info->mipLevels;
    image->layers = info->arrayLayers;
    image->block_bytes = info->format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
        info->format == VK_FORMAT_BC4_UNORM_BLOCK || info->format == VK_FORMAT_BC4_SNORM_BLOCK ? 8 : 16;
    for (uint32_t mip = 0; mip < info->mipLevels; ++mip) {
        image->mip_offset[mip] = image->block_size;
        VkExtent3D extent = hybris_bc_image_extent(image, mip);
        uint64_t blocks = ((extent.width + UINT64_C(3)) / 4) * ((extent.height + UINT64_C(3)) / 4);
        if (blocks > UINT64_MAX / image->block_bytes / image->layers)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        uint64_t bytes = blocks * image->block_bytes * image->layers;
        if (bytes > UINT64_MAX - image->block_size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        image->block_size += bytes;
    }
    return VK_SUCCESS;
}

void hybris_bc_image_backing_info(const struct hybris_bc_image *image,
    const VkImageCreateInfo *info, VkBufferCreateInfo *backing)
{
    *backing = (VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = image->block_size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = info->sharingMode, .queueFamilyIndexCount = info->queueFamilyIndexCount,
        .pQueueFamilyIndices = info->pQueueFamilyIndices};
}

VkResult hybris_bc_image_create(VkDevice device, PFN_vkGetDeviceProcAddr resolver,
    const VkPhysicalDeviceMemoryProperties *memory, const VkImageCreateInfo *info,
    unsigned rgb8_mask, const VkAllocationCallbacks *allocator, struct hybris_bc_image *image)
{
    if (!image || !device || !resolver || !memory || !info)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = hybris_bc_image_describe(info, image);
    if (result != VK_SUCCESS) return result;
    image->device = device;
    image->resolver = resolver;
    PROC(CreateImage); PROC(DestroyImage); PROC(CreateBuffer); PROC(DestroyBuffer);
    /* The fallback requires Vulkan 1.1: dedicated allocation requirements must
     * be honored independently for the decoded image and compressed backing. */
    PROC(GetImageMemoryRequirements2); PROC(GetBufferMemoryRequirements2);
    PROC(AllocateMemory); PROC(FreeMemory); PROC(BindImageMemory); PROC(BindBufferMemory);
    if (!CreateImage || !DestroyImage || !CreateBuffer || !DestroyBuffer ||
        !GetImageMemoryRequirements2 || !GetBufferMemoryRequirements2 ||
        !AllocateMemory || !FreeMemory || !BindImageMemory || !BindBufferMemory) {
        memset(image, 0, sizeof(*image));
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkImageCreateInfo decoded = *info;
    decoded.pNext = NULL;
    image->decoded_format = hybris_bc_image_format(info->format, rgb8_mask);
    decoded.format = image->decoded_format;
    decoded.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    result = CreateImage(device, &decoded, allocator, &image->image);
    if (result != VK_SUCCESS) goto fail;
    VkMemoryDedicatedRequirements dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 queried = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkImageMemoryRequirementsInfo2 request = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image->image};
    GetImageMemoryRequirements2(device, &request, &queried);
    VkMemoryRequirements requirements = queried.memoryRequirements;
    uint32_t type = memory->memoryTypeCount;
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (1u << i))) continue;
        if (type == memory->memoryTypeCount) type = i;
        if (memory->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; }
    }
    if (type == memory->memoryTypeCount) { result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto fail; }
    VkMemoryDedicatedAllocateInfo owner = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image->image};
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation ? &owner : NULL,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    result = AllocateMemory(device, &allocation, NULL, &image->decoded_memory);
    if (result != VK_SUCCESS) goto fail;
    result = BindImageMemory(device, image->image, image->decoded_memory, 0);
    if (result != VK_SUCCESS) goto fail;
    VkBufferCreateInfo backing;
    hybris_bc_image_backing_info(image, info, &backing);
    result = CreateBuffer(device, &backing, allocator, &image->blocks);
    if (result != VK_SUCCESS) goto fail;
    VkBufferMemoryRequirementsInfo2 buffer_request = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = image->blocks};
    dedicated.requiresDedicatedAllocation = dedicated.prefersDedicatedAllocation = VK_FALSE;
    GetBufferMemoryRequirements2(device, &buffer_request, &queried);
    image->requirements = queried.memoryRequirements;
    image->requires_dedicated = dedicated.requiresDedicatedAllocation;
    image->prefers_dedicated = dedicated.prefersDedicatedAllocation;
    return VK_SUCCESS;
fail:
    hybris_bc_image_destroy(image, allocator);
    return result;
}

VkResult hybris_bc_image_bind(const struct hybris_bc_image *image,
    VkDeviceMemory memory, VkDeviceSize offset)
{
    if (!image || !image->blocks) return VK_ERROR_INITIALIZATION_FAILED;
    PROC(BindBufferMemory);
    return BindBufferMemory(image->device, image->blocks, memory, offset);
}
