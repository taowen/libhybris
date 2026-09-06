/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * glibc Vulkan loader -> Android Vulkan HAL through the hybris ABI bridge.
 * The vendor allocates ICD-compatible dispatchable objects. Only the standard
 * loader owns their dispatch headers; this adapter never rewrites them.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <hybris/common/dlfcn.h>
#include "hwvulkan.h"
#include "instance.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

#define ICD_EXPORT __attribute__((visibility("default")))
static pthread_once_t hal_once = PTHREAD_ONCE_INIT;
static hwvulkan_device_t *hal;

/* HAL and common remain resident together. This is intentionally not a driver
 * unloading implementation: vendor TLS/exit callbacks can outlive a frontend. */
static void initialize_hal(void)
{
    hwvulkan_device_t *candidate = NULL;
    const hw_module_t *module = NULL;
    const char *path = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_HAL");
    if (path && path[0]) {
        void *handle = hybris_dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fprintf(stderr, "hybris ICD: cannot load HAL %s: %s\n", path, hybris_dlerror());
            return;
        }
        module = hybris_dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR);
        /* Retain the Android library reference for the lifetime of the ICD. */
    } else {
        if (hw_get_module(HWVULKAN_HARDWARE_MODULE_ID, &module) != 0) {
            fprintf(stderr, "hybris ICD: Vulkan hardware module not found\n");
            return;
        }
    }
    if (!module || module->tag != HARDWARE_MODULE_TAG ||
        !module->id || strcmp(module->id, HWVULKAN_HARDWARE_MODULE_ID) ||
        !module->methods || !module->methods->open) {
        fprintf(stderr, "hybris ICD: invalid Vulkan hardware module\n");
        return;
    }
    int result = module->methods->open(module, HWVULKAN_DEVICE_0,
                                      (hw_device_t **)&candidate);
    if (result || !candidate || candidate->common.tag != HARDWARE_DEVICE_TAG ||
        candidate->common.version != HWVULKAN_DEVICE_API_VERSION_0_1 ||
        !candidate->CreateInstance || !candidate->GetInstanceProcAddr ||
        !candidate->EnumerateInstanceExtensionProperties) {
        fprintf(stderr, "hybris ICD: incompatible Vulkan HAL device (open=%d)\n", result);
        return;
    }

    /* Android normally implements surface/swapchain operations in its loader.
     * No Android loader is present here. Reject driver-owned WSI rather than
     * pretending its surface representation matches the desktop ICD contract. */
    uint32_t count = 0;
    VkResult vr = candidate->EnumerateInstanceExtensionProperties(NULL, &count, NULL);
    if (vr != VK_SUCCESS)
        return;
    VkExtensionProperties *extensions = count ? calloc(count, sizeof(*extensions)) : NULL;
    if (count && !extensions)
        return;
    vr = candidate->EnumerateInstanceExtensionProperties(NULL, &count, extensions);
    int has_wsi = 0;
    for (uint32_t i = 0; vr == VK_SUCCESS && i < count; ++i) {
        if (!strcmp(extensions[i].extensionName, "VK_KHR_surface") ||
            !strcmp(extensions[i].extensionName, "VK_KHR_display"))
            has_wsi = 1;
    }
    free(extensions);
    if (vr != VK_SUCCESS || has_wsi) {
        fprintf(stderr, "hybris ICD: HAL extension discovery failed or needs unsupported driver-owned WSI\n");
        return;
    }
    hal = candidate;
}

static int ready(void)
{
    return pthread_once(&hal_once, initialize_hal) == 0 && hal != NULL;
}

static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    return hybris_icd_create_instance(hal, info, allocator, instance);
}

static VkResult VKAPI_CALL enumerate_instance_version(uint32_t *version)
{
    if (!ready())
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceVersion query =
        (PFN_vkEnumerateInstanceVersion)hal->GetInstanceProcAddr(VK_NULL_HANDLE,
                                                               "vkEnumerateInstanceVersion");
    if (query)
        return query(version);
    *version = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

ICD_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version)
{
    /* Version 5 lets the loader validate instance API versions. Legacy
     * loaders are deliberately unsupported, rather than assuming 1.0. */
    if (!version || *version < 5)
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    *version = 5;
    return VK_SUCCESS;
}

static const char *const physical_commands[] = {
#include "physical_commands.inc"
};

ICD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
    if (!name || !instance || !ready())
        return NULL;
    size_t low = 0, high = sizeof(physical_commands) / sizeof(physical_commands[0]);
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int order = strcmp(name, physical_commands[mid]);
        if (!order)
            return hybris_icd_instance_proc(instance, name);
        if (order < 0) high = mid;
        else low = mid + 1;
    }
    return NULL;
}

ICD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!name)
        return NULL;
    if (!strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion"))
        return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    if (!strcmp(name, "vk_icdGetPhysicalDeviceProcAddr"))
        return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    if (!ready())
        return NULL;
    if (!strcmp(name, "vkEnumerateInstanceVersion"))
        return (PFN_vkVoidFunction)enumerate_instance_version;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)hal->EnumerateInstanceExtensionProperties;
    /* Instance-local lookup preserves the HAL's command/extension scope.
     * Global queries have no object state. */
    PFN_vkVoidFunction backend = instance ? hybris_icd_instance_proc(instance, name)
                                         : hal->GetInstanceProcAddr(instance, name);
    if (backend && !strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    return backend;
}
