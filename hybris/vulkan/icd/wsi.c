/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "config.h"
#ifdef WANT_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include "wsi.h"
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef WANT_WAYLAND
#include <hybris/gralloc/gralloc.h>
#include "../platforms/wayland/window_owner.h"
#endif

#ifdef WANT_WAYLAND
static const VkExtensionProperties local_wsi[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_SPEC_VERSION},
};
#endif

#ifdef WANT_WAYLAND
struct surface_state {
    VkInstance instance;
    uint64_t generation;
    struct hybris_vk_wayland_window *window;
    struct wl_display *display;
    VkAllocationCallbacks allocator;
    int custom_allocator;
    struct surface_state *next;
};

static pthread_mutex_t surface_guard = PTHREAD_MUTEX_INITIALIZER;
static struct surface_state *surfaces;
static pthread_once_t gralloc_once = PTHREAD_ONCE_INIT;

static void initialize_gralloc(void)
{
    hybris_gralloc_initialize(0);
}

static void *surface_alloc(const VkAllocationCallbacks *allocator, size_t size)
{
    return allocator
        ? allocator->pfnAllocation(allocator->pUserData, size,
              _Alignof(struct surface_state), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
        : malloc(size);
}

static void surface_free(struct surface_state *state)
{
    if (state->custom_allocator)
        state->allocator.pfnFree(state->allocator.pUserData, state);
    else
        free(state);
}

static struct surface_state *unlink_surface(VkInstance instance, VkSurfaceKHR surface)
{
    pthread_mutex_lock(&surface_guard);
    struct surface_state **link = &surfaces;
    while (*link && (VkSurfaceKHR)(uintptr_t)*link != surface)
        link = &(*link)->next;
    struct surface_state *state = *link;
    if (state && state->instance == instance) *link = state->next;
    else state = NULL;
    pthread_mutex_unlock(&surface_guard);
    return state;
}

static struct surface_state *find_surface(VkSurfaceKHR surface)
{
    pthread_mutex_lock(&surface_guard);
    struct surface_state *state = surfaces;
    while (state && (VkSurfaceKHR)(uintptr_t)state != surface)
        state = state->next;
    pthread_mutex_unlock(&surface_guard);
    return state;
}


#endif

VkResult hybris_icd_wsi_enumerate(hwvulkan_device_t *hal, const char *layer,
    uint32_t *count, VkExtensionProperties *properties)
{
    if (!hal || !count) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer)
        return hal->EnumerateInstanceExtensionProperties(layer, count, properties);
    uint32_t hal_count = 0;
    VkResult result = hal->EnumerateInstanceExtensionProperties(NULL, &hal_count, NULL);
    if (result != VK_SUCCESS) return result;
#ifdef WANT_WAYLAND
    const uint32_t extra = sizeof(local_wsi) / sizeof(local_wsi[0]);
#else
    const uint32_t extra = 0;
#endif
    uint32_t total = hal_count + extra;
    if (!properties) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t space = *count;
    uint32_t written = 0;
    if (hal_count) {
        VkExtensionProperties *hal_props = calloc(hal_count, sizeof(*hal_props));
        if (!hal_props) return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t query = hal_count;
        result = hal->EnumerateInstanceExtensionProperties(NULL, &query, hal_props);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            free(hal_props);
            return result;
        }
        written = space < query ? space : query;
        if (written) memcpy(properties, hal_props, written * sizeof(*properties));
        free(hal_props);
    }
#ifdef WANT_WAYLAND
    for (uint32_t i = 0; i < extra && written < space; ++i)
        properties[written++] = local_wsi[i];
#endif
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult hybris_icd_wsi_prepare_instance(const VkInstanceCreateInfo *info,
    VkInstanceCreateInfo *filtered, const char ***names, int *surface_enabled,
    int *wayland_enabled)
{
    *filtered = *info;
    *names = NULL;
    *surface_enabled = 0;
    *wayland_enabled = 0;
    if (!info->enabledExtensionCount) return VK_SUCCESS;
#ifdef WANT_WAYLAND
    const char **kept = malloc(info->enabledExtensionCount * sizeof(*kept));
    if (!kept) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t count = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char *name = info->ppEnabledExtensionNames[i];
        if (!strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME)) {
            *surface_enabled = 1;
            continue;
        }
        if (!strcmp(name, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)) {
            *wayland_enabled = 1;
            continue;
        }
        kept[count++] = name;
    }
    *names = kept;
    filtered->enabledExtensionCount = count;
    filtered->ppEnabledExtensionNames = count ? kept : NULL;
#else
    (void)names;
#endif
    return VK_SUCCESS;
}

void hybris_icd_wsi_finish_instance(const char **names)
{
    free((void *)names);
}

#ifdef WANT_WAYLAND
static VkResult destroy_owned(struct surface_state *state)
{
    if (!state) return VK_SUCCESS;
    hybris_vk_wayland_window_destroy(state->window);
    surface_free(state);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_wayland_surface(VkInstance instance,
    const VkWaylandSurfaceCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    int wayland_enabled = 0;
    uint64_t generation = 0;
    if (!info || !surface ||
        info->sType != VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!hybris_icd_lookup_instance_wsi(instance, NULL, &wayland_enabled, &generation) ||
        !wayland_enabled)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!info->display || !info->surface) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_once(&gralloc_once, initialize_gralloc);
    struct hybris_vk_wayland_window *window = NULL;
    int error = hybris_vk_wayland_window_create(info->display, info->surface, &window);
    if (error)
        return error == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN;
    struct surface_state *state = surface_alloc(allocator, sizeof(*state));
    if (!state) {
        hybris_vk_wayland_window_destroy(window);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    state->instance = instance;
    state->generation = generation;
    state->window = window;
    state->display = info->display;
    state->custom_allocator = allocator != NULL;
    if (allocator) state->allocator = *allocator;
    pthread_mutex_lock(&surface_guard);
    state->next = surfaces;
    surfaces = state;
    pthread_mutex_unlock(&surface_guard);
    *surface = (VkSurfaceKHR)(uintptr_t)state;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_surface(VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    if (!instance || !surface) return;
    destroy_owned(unlink_surface(instance, surface));
}

static struct surface_state *owned_surface(VkPhysicalDevice physical,
    VkSurfaceKHR surface, struct hybris_icd_physical *context)
{
    if (!hybris_icd_lookup_physical(physical, context)) return NULL;
    struct surface_state *state = find_surface(surface);
    if (!state || state->instance != context->instance ||
        state->generation != context->generation)
        return NULL;
    return state;
}

int hybris_icd_physical_has_native_buffer(VkPhysicalDevice physical)
{
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return 0;
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)
        context.resolver(context.instance, "vkEnumerateDeviceExtensionProperties");
    if (!enumerate) return 0;
    uint32_t count = 0;
    if (enumerate(physical, NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *extensions = count ? calloc(count, sizeof(*extensions)) : NULL;
    if (count && !extensions) return 0;
    if (enumerate(physical, NULL, &count, extensions) != VK_SUCCESS) {
        free(extensions);
        return 0;
    }
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, "VK_ANDROID_native_buffer"))
            found = 1;
    free(extensions);
    return found;
}

int hybris_icd_wsi_graphics_family(VkPhysicalDevice physical, uint32_t index)
{
    struct hybris_icd_physical context;
    if (!hybris_icd_lookup_physical(physical, &context)) return 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties query =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!query) return 0;
    uint32_t count = 0;
    query(physical, &count, NULL);
    if (index >= count) return 0;
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) return 0;
    query(physical, &count, families);
    int ok = families[index].queueCount && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    free(families);
    return ok;
}

struct hybris_vk_wayland_window *hybris_icd_wsi_surface_window(VkSurfaceKHR surface,
    VkInstance *instance, uint64_t *generation)
{
    struct surface_state *state = find_surface(surface);
    if (!state) return NULL;
    if (instance) *instance = state->instance;
    if (generation) *generation = state->generation;
    return state->window;
}

static int present_engine(VkPhysicalDevice physical, uint32_t queue_family)
{
    return hybris_icd_physical_has_native_buffer(physical) &&
        hybris_icd_wsi_graphics_family(physical, queue_family);
}

static VkBool32 VKAPI_CALL wayland_presentation_support(VkPhysicalDevice physical,
    uint32_t queue_family, struct wl_display *display)
{
    struct hybris_icd_physical context;
    if (!display || !hybris_icd_lookup_physical(physical, &context))
        return VK_FALSE;
    return present_engine(physical, queue_family) ? VK_TRUE : VK_FALSE;
}

static VkResult VKAPI_CALL surface_support(VkPhysicalDevice physical,
    uint32_t queue_family, VkSurfaceKHR surface, VkBool32 *supported)
{
    struct hybris_icd_physical context;
    if (!supported) return VK_ERROR_INITIALIZATION_FAILED;
    if (!owned_surface(physical, surface, &context))
        return VK_ERROR_SURFACE_LOST_KHR;
    *supported = present_engine(physical, queue_family) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

static VkResult fill_array(uint32_t available, uint32_t *count, void *out,
    const void *src, size_t element)
{
    if (!out) {
        *count = available;
        return VK_SUCCESS;
    }
    uint32_t written = *count < available ? *count : available;
    if (written) memcpy(out, src, written * element);
    *count = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL surface_capabilities(VkPhysicalDevice physical,
    VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities)
{
    struct hybris_icd_physical context;
    if (!capabilities) return VK_ERROR_INITIALIZATION_FAILED;
    if (!owned_surface(physical, surface, &context))
        return VK_ERROR_SURFACE_LOST_KHR;
    if (!hybris_icd_physical_has_native_buffer(physical))
        return VK_ERROR_UNKNOWN;
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->minImageCount = 2;
    capabilities->maxImageCount = 8;
    capabilities->currentExtent.width = 0xffffffffu;
    capabilities->currentExtent.height = 0xffffffffu;
    capabilities->minImageExtent.width = 1;
    capabilities->minImageExtent.height = 1;
    capabilities->maxImageExtent.width = 16384;
    capabilities->maxImageExtent.height = 16384;
    capabilities->maxImageArrayLayers = 1;
    capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->supportedCompositeAlpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    capabilities->supportedUsageFlags =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL get_surface_formats(VkPhysicalDevice physical,
    VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    struct hybris_icd_physical context;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!owned_surface(physical, surface, &context))
        return VK_ERROR_SURFACE_LOST_KHR;
    if (!hybris_icd_physical_has_native_buffer(physical))
        return VK_ERROR_UNKNOWN;
    PFN_vkGetPhysicalDeviceFormatProperties query =
        (PFN_vkGetPhysicalDeviceFormatProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceFormatProperties");
    if (!query) return VK_ERROR_INITIALIZATION_FAILED;
    const VkFormat candidates[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    VkSurfaceFormatKHR supported[2];
    uint32_t available = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        VkFormatProperties properties;
        query(physical, candidates[i], &properties);
        if (properties.optimalTilingFeatures &
            (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
             VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            supported[available].format = candidates[i];
            supported[available].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            ++available;
        }
    }
    if (!available) return VK_ERROR_UNKNOWN;
    return fill_array(available, count, formats, supported, sizeof(*supported));
}

static VkResult VKAPI_CALL get_surface_present_modes(VkPhysicalDevice physical,
    VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    struct hybris_icd_physical context;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!owned_surface(physical, surface, &context))
        return VK_ERROR_SURFACE_LOST_KHR;
    if (!hybris_icd_physical_has_native_buffer(physical))
        return VK_ERROR_UNKNOWN;
    const VkPresentModeKHR fifo = VK_PRESENT_MODE_FIFO_KHR;
    return fill_array(1, count, modes, &fifo, sizeof(fifo));
}

void hybris_icd_wsi_release_instance(VkInstance instance)
{
    struct surface_state *orphans = NULL;
    pthread_mutex_lock(&surface_guard);
    struct surface_state **link = &surfaces;
    while (*link) {
        if ((*link)->instance == instance) {
            struct surface_state *state = *link;
            *link = state->next;
            state->next = orphans;
            orphans = state;
        } else {
            link = &(*link)->next;
        }
    }
    pthread_mutex_unlock(&surface_guard);
    while (orphans) {
        struct surface_state *next = orphans->next;
        destroy_owned(orphans);
        orphans = next;
    }
}
#else
void hybris_icd_wsi_release_instance(VkInstance instance)
{
    (void)instance;
}
int hybris_icd_physical_has_native_buffer(VkPhysicalDevice physical)
{
    (void)physical;
    return 0;
}
int hybris_icd_wsi_graphics_family(VkPhysicalDevice physical, uint32_t index)
{
    (void)physical;
    (void)index;
    return 0;
}
struct hybris_vk_wayland_window *hybris_icd_wsi_surface_window(VkSurfaceKHR surface,
    VkInstance *instance, uint64_t *generation)
{
    (void)surface;
    (void)instance;
    (void)generation;
    return NULL;
}
#endif

PFN_vkVoidFunction hybris_icd_wsi_proc(const char *name, int surface_enabled,
    int wayland_enabled)
{
#ifdef WANT_WAYLAND
    if (wayland_enabled) {
        if (!strcmp(name, "vkCreateWaylandSurfaceKHR"))
            return (PFN_vkVoidFunction)create_wayland_surface;
        if (!strcmp(name, "vkGetPhysicalDeviceWaylandPresentationSupportKHR"))
            return (PFN_vkVoidFunction)wayland_presentation_support;
    }
    if (surface_enabled) {
        if (!strcmp(name, "vkDestroySurfaceKHR"))
            return (PFN_vkVoidFunction)destroy_surface;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR"))
            return (PFN_vkVoidFunction)surface_support;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return (PFN_vkVoidFunction)surface_capabilities;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR"))
            return (PFN_vkVoidFunction)get_surface_formats;
        if (!strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR"))
            return (PFN_vkVoidFunction)get_surface_present_modes;
    }
#endif
    (void)name;
    (void)surface_enabled;
    (void)wayland_enabled;
    return NULL;
}
