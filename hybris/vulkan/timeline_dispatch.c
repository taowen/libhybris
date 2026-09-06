/* SPDX-License-Identifier: Apache-2.0 */
#include "render_dispatch.h"
#include <string.h>

/* Host timeline commands carry their VkDevice directly. Resolve the exact
 * core/KHR name for that registered device, outside the registry mutex: a
 * blocking host wait must not prevent another thread from signaling it. */
#define TIMELINE_WRAPPERS(suffix) \
VkResult vkGetSemaphoreCounterValue##suffix(VkDevice device, VkSemaphore semaphore, uint64_t *value) { \
    PFN_vkGetSemaphoreCounterValue call = (PFN_vkGetSemaphoreCounterValue) \
        hybris_frontend_device_command(device, "vkGetSemaphoreCounterValue" #suffix); \
    return call ? call(device, semaphore, value) : VK_ERROR_EXTENSION_NOT_PRESENT; \
} \
VkResult vkWaitSemaphores##suffix(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) { \
    PFN_vkWaitSemaphores call = (PFN_vkWaitSemaphores) \
        hybris_frontend_device_command(device, "vkWaitSemaphores" #suffix); \
    return call ? call(device, info, timeout) : VK_ERROR_EXTENSION_NOT_PRESENT; \
} \
VkResult vkSignalSemaphore##suffix(VkDevice device, const VkSemaphoreSignalInfo *info) { \
    PFN_vkSignalSemaphore call = (PFN_vkSignalSemaphore) \
        hybris_frontend_device_command(device, "vkSignalSemaphore" #suffix); \
    return call ? call(device, info) : VK_ERROR_EXTENSION_NOT_PRESENT; \
}
TIMELINE_WRAPPERS()
TIMELINE_WRAPPERS(KHR)
#undef TIMELINE_WRAPPERS

PFN_vkVoidFunction hybris_timeline_dispatch_proc(const char *name)
{
#define LOCAL(command) if (!strcmp(name, #command)) return (PFN_vkVoidFunction)command
    LOCAL(vkGetSemaphoreCounterValue); LOCAL(vkGetSemaphoreCounterValueKHR);
    LOCAL(vkWaitSemaphores); LOCAL(vkWaitSemaphoresKHR);
    LOCAL(vkSignalSemaphore); LOCAL(vkSignalSemaphoreKHR);
#undef LOCAL
    return NULL;
}
