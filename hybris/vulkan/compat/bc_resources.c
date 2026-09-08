/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <stdlib.h>
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)device->resolver(device->handle, "vk" #name)
static int emulates(struct hybris_bc_device *device, VkFormat format)
{
    return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC5_SNORM_BLOCK &&
        (device->format_mask & (1u << (format - VK_FORMAT_BC1_RGB_UNORM_BLOCK)));
}
static VkResult VKAPI_CALL create_image(VkDevice handle, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *out)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    /* Keep the WSI guard even when the BC hook takes dispatch precedence. */
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR &&
            ((const VkImageSwapchainCreateInfoKHR *)next)->swapchain)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    PROC(CreateImage);
    if (!device->format_mask) return CreateImage(handle, info, allocator, out);
    struct hybris_bc_resource *resource = hybris_bc_alloc(device, sizeof(*resource));
    if (!resource) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result;
    if (emulates(device, info->format)) {
        result = hybris_bc_image_create(handle, device->resolver, &device->memory, info,
            device->rgb8_mask, allocator, &resource->emulated);
        if (result == VK_SUCCESS) *out = resource->emulated.image;
    } else {
        result = CreateImage(handle, info, allocator, out);
    }
    if (result != VK_SUCCESS) { hybris_bc_free(device, resource); return result; }
    resource->handle = *out;
    resource->format = info->format;
    resource->type = info->imageType;
    resource->extent = info->extent;
    resource->levels = info->mipLevels;
    resource->layers = info->arrayLayers;
    resource->custom_allocator = allocator != NULL;
    if (allocator) resource->allocator = *allocator;
    pthread_mutex_lock(&device->guard);
    resource->next = device->images;
    device->images = resource;
    pthread_mutex_unlock(&device->guard);
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_image(VkDevice handle, VkImage image, const VkAllocationCallbacks *allocator)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return;
    pthread_mutex_lock(&device->guard);
    struct hybris_bc_resource **link = &device->images;
    while (*link && (*link)->handle != image) link = &(*link)->next;
    struct hybris_bc_resource *resource = *link;
    if (resource) *link = resource->next;
    pthread_mutex_unlock(&device->guard);
    if (resource && resource->emulated.image) hybris_bc_image_destroy(&resource->emulated, allocator);
    else { PROC(DestroyImage); DestroyImage(handle, image, allocator); }
    hybris_bc_free(device, resource);
}
static void VKAPI_CALL requirements(VkDevice handle, VkImage image, VkMemoryRequirements *out)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return;
    struct hybris_bc_resource *resource = hybris_bc_resource_find(device, image);
    if (resource && resource->emulated.image) *out = resource->emulated.requirements;
    else { PROC(GetImageMemoryRequirements); GetImageMemoryRequirements(handle, image, out); }
}
static void requirements2(VkDevice handle, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *out, const char *name)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return;
    struct hybris_bc_resource *resource = hybris_bc_resource_find(device, info->image);
    if (resource && resource->emulated.image) {
        PROC(GetBufferMemoryRequirements2);
        VkBufferMemoryRequirementsInfo2 buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
            .buffer = resource->emulated.blocks};
        GetBufferMemoryRequirements2(handle, &buffer, out);
    } else {
        PFN_vkGetImageMemoryRequirements2 query = (PFN_vkGetImageMemoryRequirements2)device->resolver(handle, name);
        query(handle, info, out);
    }
}
static void VKAPI_CALL requirements2_core(VkDevice handle, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *out) { requirements2(handle, info, out, "vkGetImageMemoryRequirements2"); }
static void VKAPI_CALL requirements2_khr(VkDevice handle, const VkImageMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *out) { requirements2(handle, info, out, "vkGetImageMemoryRequirements2KHR"); }
/* maintenance4 queries must describe the same compressed backing as creation,
 * without allocating either of the two internal resources. */
static void device_requirements(VkDevice handle, const VkDeviceImageMemoryRequirements *info,
    VkMemoryRequirements2 *out, const char *name)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return;
    if (!emulates(device, info->pCreateInfo->format)) {
        PFN_vkGetDeviceImageMemoryRequirements query =
            (PFN_vkGetDeviceImageMemoryRequirements)device->resolver(handle, name);
        query(handle, info, out);
        return;
    }
    struct hybris_bc_image image;
    if (hybris_bc_image_describe(info->pCreateInfo, &image) != VK_SUCCESS) {
        out->memoryRequirements = (VkMemoryRequirements){0};
        return;
    }
    VkBufferCreateInfo backing;
    hybris_bc_image_backing_info(&image, info->pCreateInfo, &backing);
    VkDeviceBufferMemoryRequirements query = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &backing};
    const char *buffer_name = !strcmp(name, "vkGetDeviceImageMemoryRequirementsKHR") ?
        "vkGetDeviceBufferMemoryRequirementsKHR" : "vkGetDeviceBufferMemoryRequirements";
    PFN_vkGetDeviceBufferMemoryRequirements get =
        (PFN_vkGetDeviceBufferMemoryRequirements)device->resolver(handle, buffer_name);
    get(handle, &query, out);
}
static void VKAPI_CALL device_requirements_core(VkDevice handle,
    const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out)
{ device_requirements(handle, info, out, "vkGetDeviceImageMemoryRequirements"); }
static void VKAPI_CALL device_requirements_khr(VkDevice handle,
    const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *out)
{ device_requirements(handle, info, out, "vkGetDeviceImageMemoryRequirementsKHR"); }
static void device_sparse_requirements(VkDevice handle, const VkDeviceImageMemoryRequirements *info,
    uint32_t *count, VkSparseImageMemoryRequirements2 *out, const char *name)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return;
    if (emulates(device, info->pCreateInfo->format)) { *count = 0; return; }
    PFN_vkGetDeviceImageSparseMemoryRequirements query =
        (PFN_vkGetDeviceImageSparseMemoryRequirements)device->resolver(handle, name);
    query(handle, info, count, out);
}
static void VKAPI_CALL device_sparse_requirements_core(VkDevice handle, const VkDeviceImageMemoryRequirements *info,
    uint32_t *count, VkSparseImageMemoryRequirements2 *out)
{ device_sparse_requirements(handle, info, count, out, "vkGetDeviceImageSparseMemoryRequirements"); }
static void VKAPI_CALL device_sparse_requirements_khr(VkDevice handle, const VkDeviceImageMemoryRequirements *info,
    uint32_t *count, VkSparseImageMemoryRequirements2 *out)
{ device_sparse_requirements(handle, info, count, out, "vkGetDeviceImageSparseMemoryRequirementsKHR"); }
static VkResult VKAPI_CALL bind_image(VkDevice handle, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct hybris_bc_resource *resource = hybris_bc_resource_find(device, image);
    if (resource && resource->emulated.image) return hybris_bc_image_bind(&resource->emulated, memory, offset);
    PROC(BindImageMemory);
    return BindImageMemory(handle, image, memory, offset);
}
static VkResult bind_images(VkDevice handle, uint32_t count, const VkBindImageMemoryInfo *infos,
    const char *name)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    /* Preflight before binding anything, including the existing WSI rejection. */
    for (uint32_t i = 0; i < count; ++i) {
        struct hybris_bc_resource *resource = hybris_bc_resource_find(device, infos[i].image);
        for (const VkBaseInStructure *next = infos[i].pNext; next; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR &&
                ((const VkBindImageMemorySwapchainInfoKHR *)next)->swapchain)
                return VK_ERROR_UNKNOWN;
            if (!resource || !resource->emulated.image) continue;
            if (next->sType == VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO) {
                const VkBindImageMemoryDeviceGroupInfo *group = (const void *)next;
                if (group->splitInstanceBindRegionCount) return VK_ERROR_FEATURE_NOT_PRESENT;
            } else if (next->sType != VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
    }
    PFN_vkBindImageMemory2 bind = (PFN_vkBindImageMemory2)device->resolver(handle, name);
    for (uint32_t i = 0; i < count; ++i) {
        struct hybris_bc_resource *resource = hybris_bc_resource_find(device, infos[i].image);
        VkResult result;
        if (resource && resource->emulated.image) {
            VkBindBufferMemoryDeviceGroupInfo group = {
                .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO};
            VkBindBufferMemoryInfo buffer = {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
                .buffer = resource->emulated.blocks, .memory = infos[i].memory,
                .memoryOffset = infos[i].memoryOffset};
            for (const VkBaseInStructure *next = infos[i].pNext; next; next = next->pNext) {
                if (next->sType != VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO) continue;
                const VkBindImageMemoryDeviceGroupInfo *source = (const void *)next;
                group.deviceIndexCount = source->deviceIndexCount;
                group.pDeviceIndices = source->pDeviceIndices;
                buffer.pNext = &group;
            }
            const char *buffer_name = !strcmp(name, "vkBindImageMemory2KHR") ?
                "vkBindBufferMemory2KHR" : "vkBindBufferMemory2";
            PFN_vkBindBufferMemory2 bind_buffer = (PFN_vkBindBufferMemory2)device->resolver(handle, buffer_name);
            result = bind_buffer(handle, 1, &buffer);
        } else {
            result = bind(handle, 1, &infos[i]);
        }
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL bind_images_core(VkDevice handle, uint32_t count, const VkBindImageMemoryInfo *infos)
{ return bind_images(handle, count, infos, "vkBindImageMemory2"); }
static VkResult VKAPI_CALL bind_images_khr(VkDevice handle, uint32_t count, const VkBindImageMemoryInfo *infos)
{ return bind_images(handle, count, infos, "vkBindImageMemory2KHR"); }
static VkResult VKAPI_CALL create_view(VkDevice handle, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct hybris_bc_resource *resource = hybris_bc_resource_find(device, info->image);
    VkImageViewCreateInfo translated = *info;
    if (resource && resource->emulated.image) {
        if (info->format != resource->format) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        translated.format = resource->emulated.decoded_format;
    }
    PROC(CreateImageView);
    return CreateImageView(handle, &translated, allocator, view);
}
/* Only the prefix leading to a dedicated-image node needs copying. The tail
 * is passed unchanged, and no application-owned pNext memory is modified. */
static size_t allocation_node_size(VkStructureType type)
{
    switch (type) {
#define NODE(name, tag) case VK_STRUCTURE_TYPE_##tag: return sizeof(Vk##name)
    NODE(MemoryAllocateFlagsInfo, MEMORY_ALLOCATE_FLAGS_INFO);
    NODE(MemoryPriorityAllocateInfoEXT, MEMORY_PRIORITY_ALLOCATE_INFO_EXT);
    NODE(MemoryOpaqueCaptureAddressAllocateInfo, MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO);
    NODE(ExportMemoryAllocateInfo, EXPORT_MEMORY_ALLOCATE_INFO);
    NODE(ImportMemoryFdInfoKHR, IMPORT_MEMORY_FD_INFO_KHR);
    NODE(ImportMemoryHostPointerInfoEXT, IMPORT_MEMORY_HOST_POINTER_INFO_EXT);
    NODE(ExportMemoryAllocateInfoNV, EXPORT_MEMORY_ALLOCATE_INFO_NV);
    NODE(MemoryDedicatedAllocateInfo, MEMORY_DEDICATED_ALLOCATE_INFO);
#undef NODE
    default: return 0;
    }
}
static VkResult VKAPI_CALL allocate_memory(VkDevice handle, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    struct hybris_bc_device *device = hybris_bc_device_find(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    PROC(AllocateMemory);
    const VkMemoryDedicatedAllocateInfo *dedicated = NULL;
    struct hybris_bc_resource *resource = NULL;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            dedicated = (const VkMemoryDedicatedAllocateInfo *)node;
            resource = hybris_bc_resource_find(device, dedicated->image);
            break;
        }
    if (!resource || !resource->emulated.image) return AllocateMemory(handle, info, allocator, memory);
    VkMemoryDedicatedAllocateInfo translated = *dedicated;
    translated.image = VK_NULL_HANDLE;
    translated.buffer = resource->emulated.blocks;
    VkBaseOutStructure *first = NULL, **tail = &first;
    VkResult result = VK_SUCCESS;
    for (const VkBaseInStructure *node = info->pNext; node != (const VkBaseInStructure *)dedicated; node = node->pNext) {
        size_t bytes = allocation_node_size(node->sType);
        if (!bytes) { result = VK_ERROR_FEATURE_NOT_PRESENT; break; }
        VkBaseOutStructure *copy = hybris_bc_alloc(device, bytes);
        if (!copy) { result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        memcpy(copy, node, bytes);
        copy->pNext = NULL;
        *tail = copy;
        tail = &copy->pNext;
    }
    if (result == VK_SUCCESS) {
        *tail = (VkBaseOutStructure *)&translated;
        VkMemoryAllocateInfo allocation = *info;
        allocation.pNext = first;
        result = AllocateMemory(handle, &allocation, allocator, memory);
        *tail = NULL;
    }
    while (first) { VkBaseOutStructure *next = first->pNext; hybris_bc_free(device, first); first = next; }
    return result;
}
PFN_vkVoidFunction hybris_bc_resources_proc(const char *name)
{
    static const struct { const char *name; PFN_vkVoidFunction function; } commands[] = {
#define ENTRY(name, function) {"vk" #name, (PFN_vkVoidFunction)function}
        ENTRY(CreateImage, create_image), ENTRY(DestroyImage, destroy_image),
        ENTRY(GetImageMemoryRequirements, requirements), ENTRY(GetImageMemoryRequirements2, requirements2_core),
        ENTRY(GetImageMemoryRequirements2KHR, requirements2_khr), ENTRY(BindImageMemory, bind_image),
        ENTRY(BindImageMemory2, bind_images_core), ENTRY(BindImageMemory2KHR, bind_images_khr),
        ENTRY(CreateImageView, create_view), ENTRY(AllocateMemory, allocate_memory),
        ENTRY(GetDeviceImageSparseMemoryRequirements, device_sparse_requirements_core),
        ENTRY(GetDeviceImageSparseMemoryRequirementsKHR, device_sparse_requirements_khr),
        ENTRY(GetDeviceImageMemoryRequirements, device_requirements_core),
        ENTRY(GetDeviceImageMemoryRequirementsKHR, device_requirements_khr)
#undef ENTRY
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(name, commands[i].name)) return commands[i].function;
    return NULL;
}
