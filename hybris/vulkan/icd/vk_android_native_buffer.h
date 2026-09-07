#ifndef HYBRIS_ICD_VK_ANDROID_NATIVE_BUFFER_H
#define HYBRIS_ICD_VK_ANDROID_NATIVE_BUFFER_H
/* Types from AOSP vk_android_native_buffer.h, used to import gralloc buffers.
 * Probe copy: tests/baseline/native_buffer_fixture.h */
#include <cutils/native_handle.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VK_ANDROID_NATIVE_BUFFER_EXTENSION_NUMBER 11
#define VK_ANDROID_NATIVE_BUFFER_SPEC_VERSION 11
#define VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME "VK_ANDROID_native_buffer"
#define VK_ANDROID_NATIVE_BUFFER_ENUM(type, id) \
    ((type)(1000000000 + (1000 * (VK_ANDROID_NATIVE_BUFFER_EXTENSION_NUMBER - 1)) + (id)))
#define VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID \
    VK_ANDROID_NATIVE_BUFFER_ENUM(VkStructureType, 0)
#define VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID \
    VK_ANDROID_NATIVE_BUFFER_ENUM(VkStructureType, 1)
typedef enum VkSwapchainImageUsageFlagBitsANDROID {
    VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID = 0x00000001
} VkSwapchainImageUsageFlagBitsANDROID;
typedef VkFlags VkSwapchainImageUsageFlagsANDROID;
typedef struct {
    uint64_t consumer;
    uint64_t producer;
} VkNativeBufferUsage2ANDROID;
typedef struct {
    VkStructureType sType;
    const void *pNext;
    buffer_handle_t handle;
    int stride;
    int format;
    int usage;
    VkNativeBufferUsage2ANDROID usage2;
    uint64_t usage3;
    struct AHardwareBuffer *ahb;
} VkNativeBufferANDROID;
typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkSwapchainImageUsageFlagsANDROID usage;
} VkSwapchainImageCreateInfoANDROID;
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainGrallocUsage2ANDROID)(VkDevice, VkFormat,
    VkImageUsageFlags, VkSwapchainImageUsageFlagsANDROID, uint64_t *, uint64_t *);
typedef VkResult (VKAPI_PTR *PFN_vkAcquireImageANDROID)(VkDevice, VkImage, int,
    VkSemaphore, VkFence);
typedef VkResult (VKAPI_PTR *PFN_vkQueueSignalReleaseImageANDROID)(VkQueue, uint32_t,
    const VkSemaphore *, VkImage, int *);
#ifdef __cplusplus
}
#endif
#endif
