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

#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hybris/common/binding.h>
#include <hybris/common/floating_point_abi.h>
#include "config.h"
#include "logging.h"
#include "vulkan_exports.h"
#include "render_dispatch.h"

static void *vulkan_handle = NULL;

static void _init_androidvulkan()
{
    vulkan_handle = (void *) android_dlopen(getenv("LIBVULKAN") ? getenv("LIBVULKAN") : "libvulkan.so", RTLD_LAZY);
    if (!vulkan_handle)
        fprintf(stderr, "libhybris vulkan: Android loader open failed: %s\n", android_dlerror());
}

static PFN_vkGetInstanceProcAddr _vkGetInstanceProcAddr;

/* Use IDLOAD approach also for float functions, since vulkan uses the aapcs-vfp calling convention even on android */

VkResult vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    if (!_vkGetInstanceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)
        _vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    return create ? create(pCreateInfo, pAllocator, pInstance)
                  : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    if (!_vkGetInstanceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)_vkGetInstanceProcAddr(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    return enumerate ? enumerate(pLayerName, pPropertyCount, pProperties)
                     : VK_ERROR_INITIALIZATION_FAILED;
}


static PFN_vkVoidFunction (*_real_vkGetDeviceProcAddr)(VkDevice device, const char* pName) = NULL;

/* Android exports can bypass extension enablement. Keep the direct ELF call
 * behind the device resolver, without any frontend window implementation. */
VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                             const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
    PFN_vkCreateSwapchainKHR create = _real_vkGetDeviceProcAddr
        ? (PFN_vkCreateSwapchainKHR)_real_vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR")
        : NULL;
    return create ? create(device, info, allocator, swapchain)
                  : VK_ERROR_EXTENSION_NOT_PRESENT;
}

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

    PFN_vkVoidFunction backend = _vkGetInstanceProcAddr(instance, pName);
    if (!backend)
        return NULL;
#define LOCAL(name) if (!strcmp(pName, #name)) return (PFN_vkVoidFunction)name
    LOCAL(vkCreateInstance);
    LOCAL(vkEnumerateInstanceExtensionProperties);
    LOCAL(vkGetDeviceProcAddr);
    LOCAL(vkEnumeratePhysicalDeviceGroupsKHR);
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
    PFN_vkVoidFunction local = hybris_render_dispatch_proc(pName);
    if (!local) local = hybris_timeline_dispatch_proc(pName);
    return local ? local : backend;
}


__attribute__((constructor))
static void _resolve_vulkan_syms(void)
{
    _init_androidvulkan();
    _vkGetInstanceProcAddr = vulkan_handle
        ? (PFN_vkGetInstanceProcAddr)android_dlsym(vulkan_handle, "vkGetInstanceProcAddr") : NULL;
    _real_vkGetDeviceProcAddr = vulkan_handle
        ? (PFN_vkGetDeviceProcAddr)android_dlsym(vulkan_handle, "vkGetDeviceProcAddr") : NULL;
    hybris_render_dispatch_init(vulkan_handle
        ? (PFN_vkCreateDevice)android_dlsym(vulkan_handle, "vkCreateDevice") : NULL,
        _real_vkGetDeviceProcAddr);
    hybris_vulkan_resolve_exports(vulkan_handle);
}

// vim:ts=4:sw=4:noexpandtab
