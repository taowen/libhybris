/*
 * Copyright (c) 2022 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* For RTLD_DEFAULT */
#define _GNU_SOURCE

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1

#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <hybris/common/binding.h>
#include <hybris/common/floating_point_abi.h>
#include "config.h"
#include "logging.h"
#include "ws.h"
#include "vulkan_exports.h"
#include "render_dispatch.h"

static void *vulkan_handle = NULL;

static void _init_androidvulkan()
{
    vulkan_handle = (void *) android_dlopen(getenv("LIBVULKAN") ? getenv("LIBVULKAN") : "libvulkan.so", RTLD_LAZY);
    if (!vulkan_handle)
        fprintf(stderr, "libhybris vulkan: Android loader open failed: %s\n", android_dlerror());
}

static inline void hybris_vulkan_initialize()
{
    _init_androidvulkan();
}

static void * _android_vulkan_dlsym(const char *symbol)
{
    if (vulkan_handle == NULL)
        _init_androidvulkan();

    return android_dlsym(vulkan_handle, symbol);
}

struct ws_vulkan_interface hybris_vulkan_interface = {
    _android_vulkan_dlsym,
};

static PFN_vkVoidFunction (*_vkGetInstanceProcAddr)(VkInstance instance, const char* pName) = NULL;
static pthread_once_t platform_proc_once = PTHREAD_ONCE_INIT;

static void initialize_platform_procs(void)
{
    ws_vkSetInstanceProcAddrFunc((PFN_vkVoidFunction)_vkGetInstanceProcAddr);
}

/* Use IDLOAD approach also for float functions, since vulkan uses the aapcs-vfp calling convention even on android */

VkResult vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    if (!_vkGetInstanceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_once(&platform_proc_once, initialize_platform_procs);

    return ws_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

VkResult vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    if (!_vkGetInstanceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_once(&platform_proc_once, initialize_platform_procs);

    return ws_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

#ifdef WANT_WAYLAND
VkResult vkCreateWaylandSurfaceKHR(VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface)
{
    return ws_vkCreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}

VkBool32 vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display)
{
    return ws_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, display);
}

void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    ws_vkDestroySurfaceKHR(instance, surface, pAllocator);
}

VkResult vkCreateXlibSurfaceKHR(VkInstance instance, const void* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkBool32 vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, void* dpy, unsigned long visualID)
{
    return VK_FALSE;
}

VkResult vkCreateXcbSurfaceKHR(VkInstance instance, const void* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkBool32 vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, void* connection, uint32_t visual_id)
{
    return VK_FALSE;
}
#endif

static PFN_vkVoidFunction (*_real_vkGetDeviceProcAddr)(VkDevice device, const char* pName) = NULL;

/* Do not resolve proc queries from our ELF export table. Android loaders may
 * export stubs for unsupported commands, and GDPA excludes instance commands.
 * Ordinary calls retain the instance/device-specific downstream pointer.
 * Only commands whose semantics we implement locally substitute a wrapper. */
/* Android loaders with a core group wrapper can still forward the KHR name
 * straight to the HAL, skipping loader initialization of physical handles.
 * Keep the KHR availability gate, then use the loader's equivalent core path.
 * Never write vendor dispatch headers from this compatibility frontend. */
VkResult vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t *count,
                                           VkPhysicalDeviceGroupProperties *groups)
{
    if (!instance || !_vkGetInstanceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumeratePhysicalDeviceGroupsKHR query = (PFN_vkEnumeratePhysicalDeviceGroupsKHR)
        _vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDeviceGroupsKHR");
    if (!query) return VK_ERROR_EXTENSION_NOT_PRESENT;
    PFN_vkEnumeratePhysicalDeviceGroups core = (PFN_vkEnumeratePhysicalDeviceGroups)
        _vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDeviceGroups");
    return core ? core(instance, count, groups) : query(instance, count, groups);
}

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (!pName)
        return NULL;
    if (!_vkGetInstanceProcAddr)
        return NULL;

    if (!strcmp(pName, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;

#ifdef WANT_WAYLAND
    /* Wayland has no downstream name. The platform translates its enabled
     * extension to Android surface at CreateInstance; query that capability. */
    const char *platform = getenv("HYBRIS_VULKANPLATFORM");
    if (!platform) platform = "wayland";
    if (!strcmp(pName, "vkCreateWaylandSurfaceKHR") ||
        !strcmp(pName, "vkGetPhysicalDeviceWaylandPresentationSupportKHR")) {
        if (!instance || strcmp(platform, "wayland") ||
            !_vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"))
            return NULL;
        return !strcmp(pName, "vkCreateWaylandSurfaceKHR")
            ? (PFN_vkVoidFunction)vkCreateWaylandSurfaceKHR
            : (PFN_vkVoidFunction)vkGetPhysicalDeviceWaylandPresentationSupportKHR;
    }
#endif
    PFN_vkVoidFunction backend = _vkGetInstanceProcAddr(instance, pName);
    if (!backend)
        return NULL;
#define LOCAL(name) if (!strcmp(pName, #name)) return (PFN_vkVoidFunction)name
    LOCAL(vkCreateInstance);
    LOCAL(vkEnumerateInstanceExtensionProperties);
    LOCAL(vkGetDeviceProcAddr);
    LOCAL(vkEnumeratePhysicalDeviceGroupsKHR);
#ifdef WANT_WAYLAND
    LOCAL(vkDestroySurfaceKHR);
    LOCAL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOCAL(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    LOCAL(vkCreateSwapchainKHR);
#endif
#undef LOCAL
    PFN_vkVoidFunction local = hybris_render_dispatch_proc(pName);
    if (!local) local = hybris_timeline_dispatch_proc(pName);
    return local ? local : backend;
}

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    if (!pName || !_real_vkGetDeviceProcAddr)
        return NULL;
    PFN_vkVoidFunction backend = _real_vkGetDeviceProcAddr(device, pName);
    if (!backend)
        return NULL;
    if (!strcmp(pName, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
#ifdef WANT_WAYLAND
    if (!strcmp(pName, "vkCreateSwapchainKHR"))
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
#endif
    PFN_vkVoidFunction local = hybris_render_dispatch_proc(pName);
    if (!local) local = hybris_timeline_dispatch_proc(pName);
    return local ? local : backend;
}

#ifdef WANT_WAYLAND
static VkResult (*_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*) = NULL;

VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    if (!_real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = _real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
    if (result == VK_SUCCESS) {
        ws_patchSurfaceCapabilities(surface, pSurfaceCapabilities);
    }
    return result;
}

VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    PFN_vkCreateSwapchainKHR create_swapchain = _real_vkGetDeviceProcAddr
        ? (PFN_vkCreateSwapchainKHR)_real_vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR")
        : NULL;
    if (!create_swapchain)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    ws_prepareSwapchain(pCreateInfo);
    return create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
}

#endif
#ifdef WANT_WAYLAND
static VkResult (*_real_vkGetPhysicalDeviceSurfaceCapabilities2KHR)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR*) = NULL;

VkResult vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, VkSurfaceCapabilities2KHR* pSurfaceCapabilities)
{
    /* A legacy query cannot populate the caller's input/output pNext chains.
     * Report the missing backend rather than returning partial success. */
    if (!_real_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = _real_vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
    if (result == VK_SUCCESS) {
        ws_patchSurfaceCapabilities(pSurfaceInfo->surface, &pSurfaceCapabilities->surfaceCapabilities);
    }
    return result;
}
#endif

__attribute__((constructor))
static void _resolve_vulkan_syms(void)
{
    _init_androidvulkan();
    _vkGetInstanceProcAddr = vulkan_handle
        ? (PFN_vkGetInstanceProcAddr)android_dlsym(vulkan_handle, "vkGetInstanceProcAddr") : NULL;
    _real_vkGetDeviceProcAddr = vulkan_handle
        ? (PFN_vkGetDeviceProcAddr)android_dlsym(vulkan_handle, "vkGetDeviceProcAddr") : NULL;
#ifdef WANT_WAYLAND
    /* Resolve Android loader trampolines once, alongside the other ELF exports.
     * Never lazily publish these pointers from concurrent physical-device calls.
     * GIPA(NULL, ...) is not a resolver for physical-device commands. */
    _real_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = vulkan_handle
        ? (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)android_dlsym(vulkan_handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") : NULL;
    _real_vkGetPhysicalDeviceSurfaceCapabilities2KHR = vulkan_handle
        ? (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)android_dlsym(vulkan_handle, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") : NULL;
#endif
    hybris_render_dispatch_init(vulkan_handle
        ? (PFN_vkCreateDevice)android_dlsym(vulkan_handle, "vkCreateDevice") : NULL,
        _real_vkGetDeviceProcAddr);
    hybris_vulkan_resolve_exports(vulkan_handle);
}

// vim:ts=4:sw=4:noexpandtab
