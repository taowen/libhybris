/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "readback.h"
#include "memory_visibility.h"
#include "application_policy.h"
#include "scaled_vertex.h"
#include <pthread.h>
#include <string.h>

struct submission {
    VkDevice device;
    uint64_t generation, serial;
    VkQueue queue;
    VkFence fence;
    VkAllocationCallbacks allocator;
    int custom, ready, cancelled;
    unsigned references;
    struct hybris_readback_write *writes;
    struct submission *next;
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
/* Waits themselves never hold this lock: wait-before-submit must remain legal.
 * Successful waiters serialize completion so a second waiter cannot return
 * while the first is still invalidating their shared readback ranges. */
static pthread_mutex_t completion_guard = PTHREAD_MUTEX_INITIALIZER;
static struct submission *submissions;
static uint64_t next_serial;

static void destroy(struct submission *s)
{
    const VkAllocationCallbacks *allocator = s->custom ? &s->allocator : NULL;
    hybris_readback_free_writes(s->writes, allocator);
    hybris_scaled_free(allocator, s);
}
static void release(struct submission *s)
{
    pthread_mutex_lock(&guard);
    int last = --s->references == 0;
    pthread_mutex_unlock(&guard);
    if (last) destroy(s);
}
static struct submission *create(const struct hybris_layer_device *device, VkQueue queue, VkFence fence)
{
    VkAllocationCallbacks allocator;
    int custom = hybris_layer_device_allocator(device->handle, &allocator);
    struct submission *s = hybris_scaled_alloc(custom ? &allocator : NULL,
        sizeof(*s), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (s) {
        *s = (struct submission){.device = device->handle, .generation = device->generation,
            .queue = queue, .fence = fence, .custom = custom};
        if (custom) s->allocator = allocator;
    }
    return s;
}
static VkResult publish(struct submission *s)
{
    pthread_mutex_lock(&guard);
    if (next_serial == UINT64_MAX) { pthread_mutex_unlock(&guard); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    s->serial = ++next_serial;
    /* The submitter retains a reference until its backend call returns. A
     * concurrent successful waiter may already retire the registry reference. */
    s->references = 2;
    s->next = submissions;
    submissions = s;
    pthread_mutex_unlock(&guard);
    return VK_SUCCESS;
}
static void submitted(struct submission *s, VkResult result)
{
    if (!s) return;
    int detached = 0;
    if (result != VK_SUCCESS) {
        pthread_mutex_lock(&guard);
        s->cancelled = 1;
        struct submission **link = &submissions;
        while (*link && *link != s) link = &(*link)->next;
        if (*link) { *link = s->next; detached = 1; }
        pthread_mutex_unlock(&guard);
    }
    if (detached) release(s);
    release(s);
}
static int owner(const struct submission *s, const struct hybris_layer_device *device)
{ return s->device == device->handle && s->generation == device->generation; }

/* A fence covers earlier submissions to its queue, including submissions
 * without fences. It says nothing about other queues on the same device. */
static struct submission *detach_completed(const struct hybris_layer_device *device,
    VkQueue queue, VkFence fence)
{
    struct submission *work = NULL;
    pthread_mutex_lock(&guard);
    if (fence) {
        for (struct submission *marker = submissions; marker; marker = marker->next)
            if (owner(marker, device) && marker->fence == fence)
                for (struct submission *s = submissions; s; s = s->next)
                    if (owner(s, device) && s->queue == marker->queue && s->serial <= marker->serial)
                        s->ready = 1;
    }
    else {
        for (struct submission *s = submissions; s; s = s->next)
            if (owner(s, device) && (!queue || s->queue == queue)) s->ready = 1;
    }
    struct submission **link = &submissions;
    while (*link) {
        struct submission *s = *link;
        if (!s->ready) { link = &s->next; continue; }
        *link = s->next;
        s->next = work;
        work = s;
    }
    pthread_mutex_unlock(&guard);
    return work;
}
static VkResult invalidate_completed(const struct hybris_layer_device *device, struct submission *work)
{
    VkResult result = VK_SUCCESS;
    while (work) {
        struct submission *next = work->next;
        VkResult current = VK_SUCCESS;
        for (const struct hybris_readback_write *w = work->writes; w && current == VK_SUCCESS; w = w->next)
            current = hybris_memory_visibility_invalidate(device, &w->range);
        if (current != VK_SUCCESS) {
            result = current;
            pthread_mutex_lock(&guard);
            int cancelled = work->cancelled;
            if (!cancelled) {
                work->ready = 0;
                work->next = submissions;
                submissions = work;
            }
            pthread_mutex_unlock(&guard);
            if (cancelled) release(work);
        }
        else release(work);
        work = next;
    }
    return result;
}
static VkResult complete_fences(VkDevice handle, uint32_t count, const VkFence *fences)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    if (!(device.application_policy & HYBRIS_APP_HOST_READBACK_INVALIDATE)) return VK_SUCCESS;
    PFN_vkGetFenceStatus status = (PFN_vkGetFenceStatus)hybris_layer_device_inner_proc(handle, "vkGetFenceStatus");
    VkResult result = VK_SUCCESS;
    pthread_mutex_lock(&completion_guard);
    for (uint32_t i = 0; i < count; ++i) {
        VkResult current = status(handle, fences[i]);
        if (current == VK_SUCCESS)
            current = invalidate_completed(&device, detach_completed(&device, VK_NULL_HANDLE, fences[i]));
        if (current != VK_SUCCESS && current != VK_NOT_READY) result = current;
    }
    pthread_mutex_unlock(&completion_guard);
    return result;
}
static VkResult complete_idle(const struct hybris_layer_device *device, VkQueue queue)
{
    pthread_mutex_lock(&completion_guard);
    VkResult result = invalidate_completed(device, detach_completed(device, queue, VK_NULL_HANDLE));
    pthread_mutex_unlock(&completion_guard);
    return result;
}
static void forget_fences(VkDevice device, uint32_t count, const VkFence *fences)
{
    pthread_mutex_lock(&guard);
    for (struct submission *s = submissions; s; s = s->next)
        if (s->device == device)
            for (uint32_t i = 0; i < count; ++i)
                if (s->fence == fences[i]) s->fence = VK_NULL_HANDLE;
    pthread_mutex_unlock(&guard);
}
void hybris_readback_release_submissions(VkDevice handle)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
    /* Device destruction is externally synchronized; no driver calls here. */
    struct submission *s = detach_completed(&device, VK_NULL_HANDLE, VK_NULL_HANDLE);
    while (s) { struct submission *next = s->next; release(s); s = next; }
}

static VkResult VKAPI_CALL queue_submit(VkQueue queue, uint32_t count,
    const VkSubmitInfo *info, VkFence fence)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_queue(queue, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    struct submission *s = NULL;
    if ((device.application_policy & HYBRIS_APP_HOST_READBACK_INVALIDATE) && (count || fence)) {
        s = create(&device, queue, fence);
        if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkResult result = VK_SUCCESS;
        for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i)
            for (uint32_t j = 0; j < info[i].commandBufferCount && result == VK_SUCCESS; ++j)
                result = hybris_readback_collect(info[i].pCommandBuffers[j], &s->writes, s->custom ? &s->allocator : NULL);
        if (result == VK_SUCCESS) result = publish(s);
        if (result != VK_SUCCESS) { destroy(s); return result; }
    }
    VkResult result = ((PFN_vkQueueSubmit)hybris_layer_device_inner_proc(device.handle, "vkQueueSubmit"))(
        queue, count, info, fence);
    submitted(s, result);
    return result;
}
static VkResult queue_submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *info,
    VkFence fence, const char *name)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_queue(queue, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    struct submission *s = NULL;
    if ((device.application_policy & HYBRIS_APP_HOST_READBACK_INVALIDATE) && (count || fence)) {
        s = create(&device, queue, fence);
        if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkResult result = VK_SUCCESS;
        for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i)
            for (uint32_t j = 0; j < info[i].commandBufferInfoCount && result == VK_SUCCESS; ++j)
                result = hybris_readback_collect(info[i].pCommandBufferInfos[j].commandBuffer,
                    &s->writes, s->custom ? &s->allocator : NULL);
        if (result == VK_SUCCESS) result = publish(s);
        if (result != VK_SUCCESS) { destroy(s); return result; }
    }
    VkResult result = ((PFN_vkQueueSubmit2)hybris_layer_device_inner_proc(device.handle, name))(queue, count, info, fence);
    submitted(s, result);
    return result;
}
static VkResult VKAPI_CALL submit2_core(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f)
{ return queue_submit2(q, n, i, f, "vkQueueSubmit2"); }
static VkResult VKAPI_CALL submit2_khr(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f)
{ return queue_submit2(q, n, i, f, "vkQueueSubmit2KHR"); }
static VkResult VKAPI_CALL wait_fences(VkDevice device, uint32_t count,
    const VkFence *fences, VkBool32 all, uint64_t timeout)
{
    VkResult result = ((PFN_vkWaitForFences)hybris_layer_device_inner_proc(device, "vkWaitForFences"))(
        device, count, fences, all, timeout);
    return result == VK_SUCCESS ? complete_fences(device, count, fences) : result;
}
static VkResult VKAPI_CALL fence_status(VkDevice device, VkFence fence)
{
    VkResult result = ((PFN_vkGetFenceStatus)hybris_layer_device_inner_proc(device, "vkGetFenceStatus"))(device, fence);
    return result == VK_SUCCESS ? complete_fences(device, 1, &fence) : result;
}
static VkResult VKAPI_CALL reset_fences(VkDevice device, uint32_t count, const VkFence *fences)
{
    VkResult result = complete_fences(device, count, fences);
    if (result != VK_SUCCESS) return result;
    result = ((PFN_vkResetFences)hybris_layer_device_inner_proc(device, "vkResetFences"))(device, count, fences);
    if (result == VK_SUCCESS) forget_fences(device, count, fences);
    return result;
}
static void VKAPI_CALL destroy_fence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator)
{
    forget_fences(device, 1, &fence);
    ((PFN_vkDestroyFence)hybris_layer_device_inner_proc(device, "vkDestroyFence"))(device, fence, allocator);
}
static VkResult VKAPI_CALL queue_idle(VkQueue queue)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_queue(queue, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = ((PFN_vkQueueWaitIdle)hybris_layer_device_inner_proc(device.handle, "vkQueueWaitIdle"))(queue);
    return result == VK_SUCCESS ? complete_idle(&device, queue) : result;
}
static VkResult VKAPI_CALL device_idle(VkDevice handle)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = ((PFN_vkDeviceWaitIdle)hybris_layer_device_inner_proc(handle, "vkDeviceWaitIdle"))(handle);
    return result == VK_SUCCESS ? complete_idle(&device, VK_NULL_HANDLE) : result;
}
PFN_vkVoidFunction hybris_readback_submit_proc(const char *name)
{
#define PROC(n, fn) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)fn
    PROC(QueueSubmit, queue_submit); PROC(QueueSubmit2, submit2_core); PROC(QueueSubmit2KHR, submit2_khr);
    PROC(WaitForFences, wait_fences); PROC(GetFenceStatus, fence_status); PROC(ResetFences, reset_fences);
    PROC(DestroyFence, destroy_fence); PROC(QueueWaitIdle, queue_idle); PROC(DeviceWaitIdle, device_idle);
#undef PROC
    return NULL;
}
