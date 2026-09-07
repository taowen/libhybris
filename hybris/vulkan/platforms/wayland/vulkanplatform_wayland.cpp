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
 */

#include <android-config.h>
#include <assert.h>
#include <errno.h>
#include <map>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ws.h"

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1
extern "C" {
#include <vulkanplatformcommon.h>
};
#include <vulkanhybris.h>

extern "C" {
#include <wayland-client.h>
#include <wayland-egl.h>
}

#include <vulkan/vulkan.h>

#include <hybris/gralloc/gralloc.h>
#include <hybris/common/binding.h>
#include "logging.h"
#include "server_wlegl_buffer.h"
#include "wayland-android-client-protocol.h"
#include "window_owner.h"

static bool init_done = false;

/* Keep track of active Vulkan window surfaces */
static std::map<VkSurfaceKHR,struct hybris_vk_wayland_window *> _surface_window_map;
static std::mutex surface_map_guard;
/* The lock protects independent surfaces. Vulkan external synchronization
 * still governs use versus destruction of the same surface. Never hold this
 * lock across driver, native-window or Wayland calls. */

int vulkan_wayland_has_mapping(VkSurfaceKHR surface)
{
    std::lock_guard<std::mutex> lock(surface_map_guard);
    return (_surface_window_map.find(surface) != _surface_window_map.end());
}

void vulkan_wayland_push_mapping(VkSurfaceKHR surface, struct hybris_vk_wayland_window *wdpy)
{
    std::lock_guard<std::mutex> lock(surface_map_guard);
    assert(_surface_window_map.find(surface) == _surface_window_map.end());

    _surface_window_map[surface] = wdpy;
}

struct hybris_vk_wayland_window *vulkan_wayland_pop_mapping(VkSurfaceKHR surface)
{
    std::lock_guard<std::mutex> lock(surface_map_guard);
    std::map<VkSurfaceKHR, struct hybris_vk_wayland_window *>::iterator it;
    it = _surface_window_map.find(surface);

    if (it == _surface_window_map.end()) return NULL;

    struct hybris_vk_wayland_window *result = it->second;
    _surface_window_map.erase(it);
    return result;
}

struct hybris_vk_wayland_window *vulkan_wayland_get_mapping(VkSurfaceKHR surface)
{
    std::lock_guard<std::mutex> lock(surface_map_guard);
    std::map<VkSurfaceKHR, struct hybris_vk_wayland_window *>::iterator it;
    it = _surface_window_map.find(surface);
    if (it == _surface_window_map.end())
        return NULL;
    return it->second;
}

static VkResult (*_vkEnumerateInstanceExtensionProperties)(const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) = NULL;
static VkResult (*_vkCreateInstance)(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) = NULL;
static PFN_vkVoidFunction (*_vkGetInstanceProcAddr)(VkInstance instance, const char *pName) = NULL;

extern "C" void waylandws_init_module(struct ws_vulkan_interface *vulkan_iface)
{
    if (init_done) {
        return;
    }
    hybris_gralloc_initialize(0);
    vulkanplatformcommon_init(vulkan_iface);
    init_done = true;
}

static VkResult waylandws_vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    VkResult res;

    if (!_vkEnumerateInstanceExtensionProperties)
        return VK_ERROR_INITIALIZATION_FAILED;

    res = (*_vkEnumerateInstanceExtensionProperties)(pLayerName, pPropertyCount, pProperties);
    if (res == VK_SUCCESS && *pPropertyCount > 0 && pProperties != NULL) {
        // Find and replace Android surface extension with wayland surface extension
        uint32_t i;
        for (i = 0; i < *pPropertyCount; i++) {
            if (strcmp(pProperties[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) == 0) {
                strncpy(pProperties[i].extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE);
            }
        }
    }
    return res;
}

VkResult waylandws_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
    if (!_vkCreateInstance)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkInstanceCreateInfo createInfo = *pCreateInfo;
    VkResult result;
    // Temporary array to replace wayland surface extension with Android surface extension
    char **enabledExtensions = (char **)malloc(pCreateInfo->enabledExtensionCount * sizeof(char *));
    uint32_t i;

    for (i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        enabledExtensions[i] = (char *)malloc(VK_MAX_EXTENSION_NAME_SIZE * sizeof(char));
        if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0) {
            strncpy(enabledExtensions[i], VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE);
        } else {
            strncpy(enabledExtensions[i], pCreateInfo->ppEnabledExtensionNames[i], VK_MAX_EXTENSION_NAME_SIZE);
        }
    }
    createInfo.ppEnabledExtensionNames = enabledExtensions;

    // Call actual vkCreateInstance
    result = (*_vkCreateInstance)(&createInfo, pAllocator, pInstance);

    // Free temporary array
    for (i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        free(enabledExtensions[i]);
    }
    free(enabledExtensions);

    return result;
}

static VkResult waylandws_vkCreateWaylandSurfaceKHR(VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface)
{
    PFN_vkCreateAndroidSurfaceKHR create_surface = _vkGetInstanceProcAddr
        ? (PFN_vkCreateAndroidSurfaceKHR)_vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR")
        : NULL;
    if (!create_surface)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR createInfo;
    VkResult result;
    hybris_vk_wayland_window *wdpy = NULL;
    int error = hybris_vk_wayland_window_create(pCreateInfo->display, pCreateInfo->surface, &wdpy);
    if (error) {
        HYBRIS_ERROR("Wayland native window creation failed: %d", error);
        return error == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN;
    }

    HYBRIS_TRACE_BEGIN("hybris-vulkan", "vkCreateWaylandSurfaceKHR", "");
    HYBRIS_TRACE_BEGIN("native-vulkan", "vkCreateWaylandSurfaceKHR", "");

    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = NULL;
    createInfo.flags = 0;
    createInfo.window = hybris_vk_wayland_window_native(wdpy);

    result = create_surface(instance, &createInfo, pAllocator, pSurface);

    HYBRIS_TRACE_END("native-vulkan", "vkCreateWaylandSurfaceKHR", "");

    if (result == VK_SUCCESS) {
        vulkan_wayland_push_mapping(*pSurface, wdpy);
    } else {
        HYBRIS_ERROR("vkCreateAndroidSurfaceKHR failed");
        hybris_vk_wayland_window_destroy(wdpy);
    }

    HYBRIS_TRACE_END("hybris-vulkan", "vkCreateWaylandSurfaceKHR", "");
    return result;
}

static VkBool32 waylandws_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display)
{
    return VK_TRUE;
}

static void waylandws_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    hybris_vk_wayland_window *wdpy = vulkan_wayland_pop_mapping(surface);
    if (wdpy) {
        PFN_vkDestroySurfaceKHR destroy_surface = _vkGetInstanceProcAddr
            ? (PFN_vkDestroySurfaceKHR)_vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR")
            : NULL;
        if (!destroy_surface) {
            fprintf(stderr, "libhybris vulkan: no vkDestroySurfaceKHR for instance\n");
            abort();
        }
        destroy_surface(instance, surface, pAllocator);
        hybris_vk_wayland_window_destroy(wdpy);
    }
}

extern "C" void waylandws_vkSetInstanceProcAddrFunc(PFN_vkVoidFunction addr)
{
    /* Called under the frontend's platform_proc_once before either global
     * operation is exposed to concurrent callers. No lazy cache writes. */
    if (_vkGetInstanceProcAddr == NULL) {
        _vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)addr;
        _vkCreateInstance = (PFN_vkCreateInstance)
            _vkGetInstanceProcAddr(NULL, "vkCreateInstance");
        _vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
            _vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    }
}

static void waylandws_patchSurfaceCapabilities(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    // Vulkan spec for Wayland: currentExtent should be undefined (0xFFFFFFFF),
    // letting the app choose its own size via vkCreateSwapchainKHR.
    // prepareSwapchain will resize the WaylandNativeWindow to match.
    pSurfaceCapabilities->currentExtent.width = 0xFFFFFFFF;
    pSurfaceCapabilities->currentExtent.height = 0xFFFFFFFF;

    if (pSurfaceCapabilities->maxImageExtent.width < 16384)
        pSurfaceCapabilities->maxImageExtent.width = 16384;
    if (pSurfaceCapabilities->maxImageExtent.height < 16384)
        pSurfaceCapabilities->maxImageExtent.height = 16384;
}

static void waylandws_prepareSwapchain(const VkSwapchainCreateInfoKHR* pCreateInfo)
{
    struct hybris_vk_wayland_window *wdpy = vulkan_wayland_get_mapping(pCreateInfo->surface);
    if (!wdpy)
        return;

    unsigned int width = pCreateInfo->imageExtent.width;
    unsigned int height = pCreateInfo->imageExtent.height;
    if (width > 0 && height > 0) {
        hybris_vk_wayland_window_resize(wdpy, width, height);
    }
}

struct ws_module ws_module_info = {
    waylandws_init_module,

    waylandws_vkEnumerateInstanceExtensionProperties,
    waylandws_vkCreateInstance,
    waylandws_vkCreateWaylandSurfaceKHR,
    waylandws_vkGetPhysicalDeviceWaylandPresentationSupportKHR,
    waylandws_vkDestroySurfaceKHR,
    waylandws_patchSurfaceCapabilities,
    waylandws_prepareSwapchain,
    waylandws_vkSetInstanceProcAddrFunc,
};
