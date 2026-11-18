/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Timeline semaphores on a driver without them (VKB_EMU_TIMELINE).
 *
 * Arm's Vulkan 1.1 drivers before r20 (Mali-G52 r18p0 on the Kirin 810, Mali-G76 r19p0 on the
 * Exynos 880) have no VK_KHR_timeline_semaphore. gamescope needs Vulkan 1.2 with
 * timeline semaphores (it numbers its submissions on one, waits on it from the CPU and reads its
 * value to recycle command buffers), and Zink asks for them too. The bridge keeps the timeline
 * itself; the driver only ever sees binary semaphores and fences:
 *   - vkCreateSemaphore with VK_SEMAPHORE_TYPE_TIMELINE: a binary semaphore is made (its handle is
 *     the client's), never given to the driver in a submit; the timeline (its value, and the
 *     signal operations still on the GPU) is kept here.
 *   - a submit that signals a timeline: its batches go to the driver with a fence of ours; the
 *     value becomes the signalled one when that fence has signalled. A fence the app passed is
 *     signalled by an empty submit after them (it then covers all of them, as before).
 *   - a submit that waits on a timeline value: already reached, the wait is dropped; signalled by
 *     work already on the GPU, the submit waits for that work's fence first (the batches before it
 *     are on the GPU already, so this never waits on itself); not signalled by anything yet (a
 *     wait-before-signal), it waits until a host signal or another thread's submit makes it so.
 *   - vkWaitSemaphores waits on the fences of the signals that satisfy it (or for a host signal),
 *     vkGetSemaphoreCounterValue and vkSignalSemaphore read and move the value.
 * Waiting in the submit call approximates ordered submissions and can block a caller
 * that has not submitted the signal yet. Sparse binding and external timeline payloads
 * are outside this compatibility path.
 *
 * Ported from MaliBridge server/server_timeline.c (800b5f5).
 */
#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include "timeline.h"
#include "../../layer/layer.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TL_BUCKETS 64
#define TL_SLICE_NS 20000000ull /* a waiter looks at new signals at least this often */

typedef struct tl_fence {
    VkFence fence;
    int refs;       /* points and waiters holding it */
    int done;       /* seen signalled */
    struct tl_fence *next;
} tl_fence;

typedef struct tl_point {
    uint64_t value;
    tl_fence *f;
    struct tl_point *next;
} tl_point;

typedef struct tl_sem {
    uint64_t handle;
    uint64_t value;     /* reached */
    tl_point *pending;  /* signals on the GPU */
    struct tl_sem *next;
} tl_sem;

typedef struct hybris_timeline {
    pthread_mutex_t lock;
    pthread_cond_t cond; /* a value moved or a signal was submitted */
    tl_sem *sems[TL_BUCKETS];
    tl_fence *free_fences;
    VkDevice dev;
    PFN_vkCreateSemaphore create_sem;
    PFN_vkDestroySemaphore destroy_sem;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    PFN_vkGetSemaphoreFdKHR get_fd;
    PFN_vkImportSemaphoreFdKHR import_fd;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkWaitForFences wait_fences;
} hybris_timeline;

static struct hybris_timeline *device_timeline(VkDevice handle)
{
    struct hybris_layer_device device;
    return hybris_layer_lookup_device(handle, &device) ? device.timeline : NULL;
}

static struct hybris_timeline *queue_timeline(VkQueue queue)
{
    struct hybris_layer_device device;
    return hybris_layer_lookup_queue(queue, &device) ? device.timeline : NULL;
}

static PFN_vkVoidFunction native_proc(VkDevice handle, const char *name, const char *alias)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return NULL;
    PFN_vkVoidFunction proc = device.resolver(handle, name);
    return proc || !alias ? proc : device.resolver(handle, alias);
}

static unsigned bucket(uint64_t h)
{
    return (unsigned)((h * 0x9E3779B97F4A7C15ull) >> 58) & (TL_BUCKETS - 1);
}

static tl_sem *sem_find(hybris_timeline *tl, uint64_t h)
{
    for (tl_sem *s = tl->sems[bucket(h)]; s; s = s->next)
        if (s->handle == h) return s;
    return NULL;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A fence of ours, unsignalled (lock held): a free one that has signalled, reset, else a new one. */
static tl_fence *fence_get(hybris_timeline *tl)
{
    tl_fence *f = NULL;
    for (tl_fence **pp = &tl->free_fences; *pp; pp = &(*pp)->next)
        if ((*pp)->done || tl->fence_status(tl->dev, (*pp)->fence) == VK_SUCCESS) {
            f = *pp;
            *pp = f->next;
            if (tl->reset_fences(tl->dev, 1, &f->fence) != VK_SUCCESS) {
                tl->destroy_fence(tl->dev, f->fence, NULL);
                free(f);
                f = NULL;
            }
            break;
        }
    if (!f) {
        f = calloc(1, sizeof(*f));
        if (!f) return NULL;
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (tl->create_fence(tl->dev, &ci, NULL, &f->fence) != VK_SUCCESS) {
            free(f);
            return NULL;
        }
    }
    f->refs = 0;
    f->done = 0;
    f->next = NULL;
    return f;
}

/* One holder fewer (lock held); with none left the fence is free again (fence_get takes it once
 * it has signalled). */
static void fence_put(hybris_timeline *tl, tl_fence *f)
{
    if (--f->refs > 0) return;
    f->next = tl->free_fences;
    tl->free_fences = f;
}

static int fence_signalled(hybris_timeline *tl, tl_fence *f)
{
    if (!f->done && tl->fence_status(tl->dev, f->fence) == VK_SUCCESS) f->done = 1;
    return f->done;
}

/* The value from the signals that have finished (lock held). */
static void sem_update(hybris_timeline *tl, tl_sem *s)
{
    tl_point **pp = &s->pending;
    while (*pp) {
        tl_point *p = *pp;
        if (p->value <= s->value || fence_signalled(tl, p->f)) {
            if (p->value > s->value && p->f->done) s->value = p->value;
            *pp = p->next;
            fence_put(tl, p->f);
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

/* The first signal on the GPU that reaches v (lock held), NULL if none. */
static tl_point *sem_reaching(tl_sem *s, uint64_t v)
{
    tl_point *best = NULL;
    for (tl_point *p = s->pending; p; p = p->next)
        if (p->value >= v && (!best || p->value < best->value)) best = p;
    return best;
}

/* ------------------------------------------------------------------ semaphores */

static VKAPI_ATTR VkResult VKAPI_CALL tl_CreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a,
                                                         VkSemaphore *out)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkCreateSemaphore call = (PFN_vkCreateSemaphore)native_proc(d, "vkCreateSemaphore", NULL);
        return call ? call(d, ci, a, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    const VkSemaphoreTypeCreateInfo *ty = NULL;
    for (const VkBaseInStructure *b = ci ? ci->pNext : NULL; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) ty = (const VkSemaphoreTypeCreateInfo *)b;
    if (!ty || ty->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE) {
        if (!ty) return tl->create_sem(d, ci, a, out);
        /* a binary one named as such: the driver does not know the structure */
        VkSemaphoreCreateInfo c = *ci;
        VkExportSemaphoreCreateInfo export_info;
        if (ci->pNext == ty) {
            c.pNext = ty->pNext;
        } else {
            const VkBaseInStructure *first = ci->pNext;
            if (first->sType != VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO ||
                first->pNext != (const void *)ty) return VK_ERROR_FEATURE_NOT_PRESENT;
            export_info = *(const VkExportSemaphoreCreateInfo *)first;
            export_info.pNext = ty->pNext;
            c.pNext = &export_info;
        }
        return tl->create_sem(d, &c, a, out);
    }
    /* the timeline's handle: a binary semaphore without export (its payload is never shared) */
    VkSemaphoreCreateInfo c = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, ci->flags};
    tl_sem *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = tl->create_sem(d, &c, a, out);
    if (r != VK_SUCCESS) {
        free(s);
        return r;
    }
    s->handle = (uint64_t)(uintptr_t)*out;
    s->value = ty->initialValue;
    pthread_mutex_lock(&tl->lock);
    unsigned b = bucket(s->handle);
    s->next = tl->sems[b];
    tl->sems[b] = s;
    pthread_mutex_unlock(&tl->lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL tl_DestroySemaphore(VkDevice d, VkSemaphore sem, const VkAllocationCallbacks *a)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkDestroySemaphore call = (PFN_vkDestroySemaphore)native_proc(d, "vkDestroySemaphore", NULL);
        if (call) call(d, sem, a);
        return;
    }
    uint64_t h = (uint64_t)(uintptr_t)sem;
    tl_sem *s = NULL;
    pthread_mutex_lock(&tl->lock);
    for (tl_sem **pp = &tl->sems[bucket(h)]; h && *pp; pp = &(*pp)->next)
        if ((*pp)->handle == h) {
            s = *pp;
            *pp = s->next;
            break;
        }
    if (s) {
        /* the app has made sure its signals are done; what the GPU still holds is let go */
        while (s->pending) {
            tl_point *p = s->pending;
            s->pending = p->next;
            fence_put(tl, p->f);
            free(p);
        }
        free(s);
    }
    pthread_mutex_unlock(&tl->lock);
    tl->destroy_sem(d, sem, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_GetSemaphoreCounterValue(VkDevice d, VkSemaphore sem, uint64_t *v)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkGetSemaphoreCounterValue call = (PFN_vkGetSemaphoreCounterValue)native_proc(d, "vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR");
        return call ? call(d, sem, v) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    pthread_mutex_lock(&tl->lock);
    tl_sem *s = sem_find(tl, (uint64_t)(uintptr_t)sem);
    if (s) {
        sem_update(tl, s);
        *v = s->value;
    }
    pthread_mutex_unlock(&tl->lock);
    return s ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_SignalSemaphore(VkDevice d, const VkSemaphoreSignalInfo *si)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkSignalSemaphore call = (PFN_vkSignalSemaphore)native_proc(d, "vkSignalSemaphore", "vkSignalSemaphoreKHR");
        return call ? call(d, si) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    pthread_mutex_lock(&tl->lock);
    tl_sem *s = sem_find(tl, (uint64_t)(uintptr_t)si->semaphore);
    if (s) {
        if (si->value > s->value) s->value = si->value;
        sem_update(tl, s);
        pthread_cond_broadcast(&tl->cond);
    }
    pthread_mutex_unlock(&tl->lock);
    return s ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

/* Wait until each (any) of n timeline values is reached, or the deadline (ns, monotonic; 0: now only). */
static VkResult wait_values(hybris_timeline *tl, uint32_t n, const VkSemaphore *sems, const uint64_t *vals, int any, uint64_t deadline)
{
    for (;;) {
        tl_fence *fs[32];
        uint32_t nf = 0;
        int done = 0;
        uint32_t unknown = 0;
        pthread_mutex_lock(&tl->lock);
        uint32_t reached = 0;
        for (uint32_t i = 0; i < n; i++) {
            tl_sem *s = sem_find(tl, (uint64_t)(uintptr_t)sems[i]);
            if (!s) { /* not a timeline (invalid usage): taken as reached */
                unknown++;
                continue;
            }
            sem_update(tl, s);
            if (s->value >= vals[i]) {
                reached++;
                continue;
            }
            tl_point *p = sem_reaching(s, vals[i]);
            if (p && nf < 32) {
                int dup = 0;
                for (uint32_t k = 0; k < nf; k++) dup |= fs[k] == p->f;
                if (!dup) p->f->refs++, fs[nf++] = p->f;
            }
        }
        reached += unknown;
        done = any ? reached > 0 : reached == n;
        uint64_t now = now_ns();
        if (done || now >= deadline) {
            for (uint32_t k = 0; k < nf; k++) fence_put(tl, fs[k]);
            pthread_mutex_unlock(&tl->lock);
            return done ? VK_SUCCESS : VK_TIMEOUT;
        }
        uint64_t slice = deadline - now < TL_SLICE_NS ? deadline - now : TL_SLICE_NS;
        if (!nf) {
            /* nothing on the GPU reaches it yet: a host signal or a submit will say so */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t ns = (uint64_t)ts.tv_nsec + slice;
            ts.tv_sec += (time_t)(ns / 1000000000ull);
            ts.tv_nsec = (long)(ns % 1000000000ull);
            pthread_cond_timedwait(&tl->cond, &tl->lock, &ts);
            pthread_mutex_unlock(&tl->lock);
            continue;
        }
        pthread_mutex_unlock(&tl->lock);
        VkFence raw[32];
        for (uint32_t k = 0; k < nf; k++) raw[k] = fs[k]->fence;
        VkResult r = tl->wait_fences(tl->dev, nf, raw, VK_FALSE, slice);
        pthread_mutex_lock(&tl->lock);
        for (uint32_t k = 0; k < nf; k++) fence_put(tl, fs[k]);
        pthread_mutex_unlock(&tl->lock);
        if (r != VK_SUCCESS && r != VK_TIMEOUT) return r;
    }
}

static uint64_t deadline_of(uint64_t timeout)
{
    uint64_t now = now_ns();
    return timeout > UINT64_MAX - now ? UINT64_MAX : now + timeout;
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkWaitSemaphores call = (PFN_vkWaitSemaphores)native_proc(d, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
        return call ? call(d, wi, timeout) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (!wi || !wi->semaphoreCount) return VK_SUCCESS;
    return wait_values(tl, wi->semaphoreCount, wi->pSemaphores, wi->pValues, (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0,
                       timeout ? deadline_of(timeout) : 0);
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_GetSemaphoreFdKHR(VkDevice d, const VkSemaphoreGetFdInfoKHR *gi, int *fd)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkGetSemaphoreFdKHR call = (PFN_vkGetSemaphoreFdKHR)native_proc(d, "vkGetSemaphoreFdKHR", NULL);
        return call ? call(d, gi, fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    pthread_mutex_lock(&tl->lock);
    int mine = gi && sem_find(tl, (uint64_t)(uintptr_t)gi->semaphore) != NULL;
    pthread_mutex_unlock(&tl->lock);
    if (mine) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    return tl->get_fd ? tl->get_fd(d, gi, fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_ImportSemaphoreFdKHR(VkDevice d, const VkImportSemaphoreFdInfoKHR *ii)
{
    hybris_timeline *tl = device_timeline(d);
    if (!tl) {
        PFN_vkImportSemaphoreFdKHR call = (PFN_vkImportSemaphoreFdKHR)native_proc(d, "vkImportSemaphoreFdKHR", NULL);
        return call ? call(d, ii) : VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    pthread_mutex_lock(&tl->lock);
    int mine = ii && sem_find(tl, (uint64_t)(uintptr_t)ii->semaphore) != NULL;
    pthread_mutex_unlock(&tl->lock);
    if (mine) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    return tl->import_fd ? tl->import_fd(d, ii) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

/* ------------------------------------------------------------------ submits */

/* One batch, either form, as the bridge needs it. */
typedef struct tl_batch {
    const void *src;
    int two; /* VkSubmitInfo2 */
} tl_batch;

static int is_timeline(hybris_timeline *tl, VkSemaphore s)
{
    return sem_find(tl, (uint64_t)(uintptr_t)s) != NULL;
}

static const VkTimelineSemaphoreSubmitInfo *timeline_info(const VkSubmitInfo *si)
{
    for (const VkBaseInStructure *b = si->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) return (const VkTimelineSemaphoreSubmitInfo *)b;
    return NULL;
}

/* Any timeline semaphore in these batches (lock held). */
static int any_timeline(hybris_timeline *tl, uint32_t n, const tl_batch *b)
{
    for (uint32_t i = 0; i < n; i++) {
        if (b[i].two) {
            const VkSubmitInfo2 *s = b[i].src;
            for (uint32_t k = 0; k < s->waitSemaphoreInfoCount; k++)
                if (is_timeline(tl, s->pWaitSemaphoreInfos[k].semaphore)) return 1;
            for (uint32_t k = 0; k < s->signalSemaphoreInfoCount; k++)
                if (is_timeline(tl, s->pSignalSemaphoreInfos[k].semaphore)) return 1;
        } else {
            const VkSubmitInfo *s = b[i].src;
            for (uint32_t k = 0; k < s->waitSemaphoreCount; k++)
                if (is_timeline(tl, s->pWaitSemaphores[k])) return 1;
            for (uint32_t k = 0; k < s->signalSemaphoreCount; k++)
                if (is_timeline(tl, s->pSignalSemaphores[k])) return 1;
        }
    }
    return 0;
}

/* The driver's copies of a run of batches: timeline waits and signals taken out (kept in `sig`). */
typedef struct tl_sig {
    VkSemaphore sem;
    uint64_t value;
} tl_sig;

typedef struct tl_run {
    uint32_t n, cap;
    VkSubmitInfo *si;
    VkSubmitInfo2 *si2;
    tl_sig *sig;
    uint32_t nsig, sigcap;
    void **owned; /* arrays to free after the submit */
    uint32_t nowned, ownedcap;
} tl_run;

static void *run_own(tl_run *r, size_t bytes)
{
    if (r->nowned == r->ownedcap) {
        uint32_t c = r->ownedcap ? r->ownedcap * 2 : 16;
        void **o = realloc(r->owned, c * sizeof(*o));
        if (!o) return NULL;
        r->owned = o;
        r->ownedcap = c;
    }
    void *p = calloc(1, bytes ? bytes : 1);
    if (p) r->owned[r->nowned++] = p;
    return p;
}

static void run_reset(tl_run *r)
{
    for (uint32_t i = 0; i < r->nowned; i++) free(r->owned[i]);
    r->nowned = 0;
    r->n = 0;
    r->nsig = 0;
}

static void run_free(tl_run *r)
{
    run_reset(r);
    free(r->owned);
    free(r->si);
    free(r->si2);
    free(r->sig);
}

static int add_sig(tl_run *r, VkSemaphore s, uint64_t v)
{
    if (r->nsig == r->sigcap) {
        uint32_t c = r->sigcap ? r->sigcap * 2 : 8;
        tl_sig *g = realloc(r->sig, c * sizeof(*g));
        if (!g) return 0;
        r->sig = g;
        r->sigcap = c;
    }
    r->sig[r->nsig++] = (tl_sig){s, v};
    return 1;
}

typedef struct tl_wait {
    VkSemaphore sem;
    uint64_t value;
} tl_wait;

/* Submit the run (with fence f: ours when it signals timelines, else the given one). */
static VkResult run_submit(hybris_timeline *tl, VkQueue q, tl_run *r, int two, VkFence fence)
{
    tl_fence *mine = NULL;
    tl_point *points = NULL;
    if (r->nsig) {
        /* Reserve every point before handing work to the GPU. */
        uint32_t remaining = r->nsig;
        while (remaining) {
            tl_point *point = calloc(1, sizeof(*point));
            if (!point) break;
            point->value = r->sig[--remaining].value;
            point->next = points;
            points = point;
        }
        if (!remaining) {
            pthread_mutex_lock(&tl->lock);
            mine = fence_get(tl);
            pthread_mutex_unlock(&tl->lock);
        }
        if (!mine) {
            while (points) {
                tl_point *next = points->next;
                free(points);
                points = next;
            }
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        fence = mine->fence;
    }
    VkResult res = two ? tl->submit2(q, r->n, r->si2, fence) : tl->submit(q, r->n, r->si, fence);
    if (mine) {
        pthread_mutex_lock(&tl->lock);
        for (uint32_t i = 0; points; i++) {
            tl_point *point = points;
            points = point->next;
            tl_sem *sem = res == VK_SUCCESS ? sem_find(tl, (uint64_t)(uintptr_t)r->sig[i].sem) : NULL;
            if (!sem) {
                free(point);
                continue;
            }
            point->f = mine;
            mine->refs++;
            point->next = sem->pending;
            sem->pending = point;
        }
        if (res != VK_SUCCESS) mine->done = 1;
        else pthread_cond_broadcast(&tl->cond);
        mine->refs++;
        fence_put(tl, mine);
        pthread_mutex_unlock(&tl->lock);
    }
    run_reset(r);
    return res;
}

/* Wait (on the CPU) until each timeline value is reached: before this the batches ahead of it went
 * to the GPU, so what reaches it is either there already or comes from another thread / the host. */
static VkResult wait_ready(hybris_timeline *tl, uint32_t n, const tl_wait *w)
{
    for (uint32_t i = 0; i < n; i++) {
        pthread_mutex_lock(&tl->lock);
        tl_sem *s = sem_find(tl, (uint64_t)(uintptr_t)w[i].sem);
        int reached = !s;
        if (s) {
            sem_update(tl, s);
            reached = s->value >= w[i].value;
        }
        pthread_mutex_unlock(&tl->lock);
        if (reached) continue;
        VkResult r = wait_values(tl, 1, &w[i].sem, &w[i].value, 0, UINT64_MAX);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

/* The timeline waits of one batch (lock held); 0 when there are more than `cap`. */
static int batch_waits(hybris_timeline *tl, const tl_batch *b, tl_wait *w, uint32_t cap, uint32_t *n)
{
    *n = 0;
    if (b->two) {
        const VkSubmitInfo2 *s = b->src;
        for (uint32_t k = 0; k < s->waitSemaphoreInfoCount; k++)
            if (is_timeline(tl, s->pWaitSemaphoreInfos[k].semaphore)) {
                if (*n == cap) return 0;
                w[(*n)++] = (tl_wait){s->pWaitSemaphoreInfos[k].semaphore, s->pWaitSemaphoreInfos[k].value};
            }
    } else {
        const VkSubmitInfo *s = b->src;
        const VkTimelineSemaphoreSubmitInfo *ti = timeline_info(s);
        for (uint32_t k = 0; k < s->waitSemaphoreCount; k++)
            if (is_timeline(tl, s->pWaitSemaphores[k])) {
                if (*n == cap) return 0;
                uint64_t v = ti && ti->pWaitSemaphoreValues && k < ti->waitSemaphoreValueCount ? ti->pWaitSemaphoreValues[k] : 0;
                w[(*n)++] = (tl_wait){s->pWaitSemaphores[k], v};
            }
    }
    return 1;
}

/* Copy only the prefix. A layer must not edit the application's chain. */
static VkResult remove_submit_timeline(tl_run *run, const VkBaseInStructure *chain,
    const VkTimelineSemaphoreSubmitInfo *timeline, const void **result)
{
    VkBaseOutStructure *head = NULL;
    VkBaseOutStructure **tail = &head;
    for (const VkBaseInStructure *node = chain; node != (const void *)timeline; node = node->pNext) {
        size_t size;
        switch (node->sType) {
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO: size = sizeof(VkDeviceGroupSubmitInfo); break;
        case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO: size = sizeof(VkProtectedSubmitInfo); break;
        case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: size = sizeof(VkPerformanceQuerySubmitInfoKHR); break;
        default: return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkBaseOutStructure *copy = run_own(run, size);
        if (!copy) return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(copy, node, size);
        *tail = copy;
        tail = &copy->pNext;
    }
    *tail = (void *)timeline->pNext;
    *result = head;
    return VK_SUCCESS;
}

/* The driver's copy of one batch into the run: binary semaphores only; the timeline signals noted
 * in the run (lock held). */
static VkResult batch_copy(hybris_timeline *tl, const tl_batch *b, tl_run *r)
{
    if (b->two) {
        const VkSubmitInfo2 *s = b->src;
        VkSubmitInfo2 c = *s;
        VkSemaphoreSubmitInfo *wv = run_own(r, s->waitSemaphoreInfoCount * sizeof(*wv));
        VkSemaphoreSubmitInfo *sv = run_own(r, s->signalSemaphoreInfoCount * sizeof(*sv));
        if (!wv || !sv) return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t a = 0, z = 0;
        for (uint32_t k = 0; k < s->waitSemaphoreInfoCount; k++)
            if (!is_timeline(tl, s->pWaitSemaphoreInfos[k].semaphore)) wv[a++] = s->pWaitSemaphoreInfos[k];
        for (uint32_t k = 0; k < s->signalSemaphoreInfoCount; k++) {
            const VkSemaphoreSubmitInfo *x = &s->pSignalSemaphoreInfos[k];
            if (!is_timeline(tl, x->semaphore)) sv[z++] = *x;
            else if (!add_sig(r, x->semaphore, x->value)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        c.waitSemaphoreInfoCount = a;
        c.pWaitSemaphoreInfos = wv;
        c.signalSemaphoreInfoCount = z;
        c.pSignalSemaphoreInfos = sv;
        r->si2[r->n++] = c;
        return VK_SUCCESS;
    }
    const VkSubmitInfo *s = b->src;
    const VkTimelineSemaphoreSubmitInfo *ti = timeline_info(s);
    VkSubmitInfo c = *s;
    if (ti) {
        VkResult result = remove_submit_timeline(r, s->pNext, ti, &c.pNext);
        if (result != VK_SUCCESS) return result;
    }
    for (const VkBaseInStructure *node = c.pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t nws = s->waitSemaphoreCount, nss = s->signalSemaphoreCount;
    VkSemaphore *ws = run_own(r, nws * sizeof(*ws));
    VkPipelineStageFlags *wm = run_own(r, nws * sizeof(*wm));
    VkSemaphore *ss = run_own(r, nss * sizeof(*ss));
    if (!ws || !wm || !ss) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t a = 0, z = 0;
    for (uint32_t k = 0; k < nws; k++)
        if (!is_timeline(tl, s->pWaitSemaphores[k])) {
            ws[a] = s->pWaitSemaphores[k];
            wm[a++] = s->pWaitDstStageMask[k];
        }
    for (uint32_t k = 0; k < nss; k++) {
        if (!is_timeline(tl, s->pSignalSemaphores[k])) {
            ss[z++] = s->pSignalSemaphores[k];
        } else {
            uint64_t v = ti && ti->pSignalSemaphoreValues && k < ti->signalSemaphoreValueCount ? ti->pSignalSemaphoreValues[k] : 0;
            if (!add_sig(r, s->pSignalSemaphores[k], v)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    c.waitSemaphoreCount = a;
    c.pWaitSemaphores = ws;
    c.pWaitDstStageMask = wm;
    c.signalSemaphoreCount = z;
    c.pSignalSemaphores = ss;
    r->si[r->n++] = c;
    return VK_SUCCESS;
}

/* Batches in runs: a batch that signals a timeline ends its run (the run's fence says when); one
 * that waits for a timeline value not reached yet first sends the runs ahead of it to the GPU, then
 * waits for the value on the CPU (wait_ready). The app's fence goes with the last run, or alone
 * after it: it signals when everything before it has finished. */
static VkResult submit_any(hybris_timeline *tl, VkQueue q, uint32_t n, const tl_batch *b, VkFence fence, int two)
{
    tl_run r;
    memset(&r, 0, sizeof(r));
    if (two) r.si2 = calloc(n ? n : 1, sizeof(*r.si2));
    else r.si = calloc(n ? n : 1, sizeof(*r.si));
    if (two ? !r.si2 : !r.si) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < n && res == VK_SUCCESS; i++) {
        tl_wait waits[64];
        uint32_t nw = 0;
        pthread_mutex_lock(&tl->lock);
        int ok = batch_waits(tl, &b[i], waits, 64, &nw);
        pthread_mutex_unlock(&tl->lock);
        if (!ok) {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }
        if (nw) {
            /* r has no timeline signals here: a batch with them was sent at once */
            if (r.n) res = run_submit(tl, q, &r, two, VK_NULL_HANDLE);
            if (res == VK_SUCCESS) res = wait_ready(tl, nw, waits);
            if (res != VK_SUCCESS) break;
        }
        pthread_mutex_lock(&tl->lock);
        res = batch_copy(tl, &b[i], &r);
        pthread_mutex_unlock(&tl->lock);
        if (res == VK_SUCCESS && r.nsig) res = run_submit(tl, q, &r, two, VK_NULL_HANDLE);
    }
    if (res == VK_SUCCESS && (r.n || fence != VK_NULL_HANDLE)) res = run_submit(tl, q, &r, two, fence);
    run_free(&r);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fence)
{
    hybris_timeline *tl = queue_timeline(q);
    if (!tl) return VK_ERROR_DEVICE_LOST;
    if (!n) return tl->submit(q, n, si, fence);
    tl_batch bs[64], *b = n <= 64 ? bs : calloc(n, sizeof(*b));
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < n; i++) b[i] = (tl_batch){&si[i], 0};
    pthread_mutex_lock(&tl->lock);
    int any = any_timeline(tl, n, b);
    pthread_mutex_unlock(&tl->lock);
    VkResult r;
    if (!any) {
        /* no timeline: as it came, the timeline structure (zero values) dropped */
        VkSubmitInfo cs[64], *c = n <= 64 ? cs : calloc(n, sizeof(*c));
        if (!c) {
            if (b != bs) free(b);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        tl_run owned = {0};
        int changed = 0;
        r = VK_SUCCESS;
        for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
            c[i] = si[i];
            const VkTimelineSemaphoreSubmitInfo *timeline = timeline_info(&si[i]);
            if (timeline) {
                changed = 1;
                r = remove_submit_timeline(&owned, si[i].pNext, timeline, &c[i].pNext);
            }
        }
        if (r == VK_SUCCESS) r = tl->submit(q, n, changed ? c : si, fence);
        run_free(&owned);
        if (c != cs) free(c);
    } else {
        r = submit_any(tl, q, n, b, fence, 0);
    }
    if (b != bs) free(b);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL tl_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence)
{
    hybris_timeline *tl = queue_timeline(q);
    if (!tl || !tl->submit2) return VK_ERROR_DEVICE_LOST;
    if (!n) return tl->submit2(q, n, si, fence);
    tl_batch bs[64], *b = n <= 64 ? bs : calloc(n, sizeof(*b));
    if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < n; i++) b[i] = (tl_batch){&si[i], 1};
    pthread_mutex_lock(&tl->lock);
    int any = any_timeline(tl, n, b);
    pthread_mutex_unlock(&tl->lock);
    VkResult r = any ? submit_any(tl, q, n, b, fence, 1) : tl->submit2(q, n, si, fence);
    if (b != bs) free(b);
    return r;
}

/* ------------------------------------------------------------------ device */

VkResult hybris_timeline_attach(struct hybris_layer_device *device)
{
    hybris_timeline *tl = calloc(1, sizeof(*tl));
    if (!tl) return VK_ERROR_OUT_OF_HOST_MEMORY;
    tl->dev = device->handle;
#define RESOLVE(field, name) tl->field = (PFN_vk##name)device->resolver(tl->dev, "vk" #name)
    RESOLVE(create_sem, CreateSemaphore);
    RESOLVE(destroy_sem, DestroySemaphore);
    RESOLVE(submit, QueueSubmit);
    RESOLVE(submit2, QueueSubmit2);
    if (!tl->submit2)
        tl->submit2 = (PFN_vkQueueSubmit2)device->resolver(tl->dev, "vkQueueSubmit2KHR");
    RESOLVE(get_fd, GetSemaphoreFdKHR);
    RESOLVE(import_fd, ImportSemaphoreFdKHR);
    RESOLVE(create_fence, CreateFence);
    RESOLVE(destroy_fence, DestroyFence);
    RESOLVE(reset_fences, ResetFences);
    RESOLVE(fence_status, GetFenceStatus);
    RESOLVE(wait_fences, WaitForFences);
#undef RESOLVE
    if (!tl->create_sem || !tl->destroy_sem || !tl->submit || !tl->create_fence ||
        !tl->destroy_fence || !tl->reset_fences || !tl->fence_status || !tl->wait_fences) {
        free(tl);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_mutex_init(&tl->lock, NULL)) {
        free(tl);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pthread_cond_init(&tl->cond, NULL)) {
        pthread_mutex_destroy(&tl->lock);
        free(tl);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    device->timeline = tl;
    return VK_SUCCESS;
}

void hybris_timeline_release(struct hybris_timeline *tl)
{
    if (!tl) return;
    for (int b = 0; b < TL_BUCKETS; b++)
        while (tl->sems[b]) {
            tl_sem *s = tl->sems[b];
            tl->sems[b] = s->next;
            while (s->pending) {
                tl_point *p = s->pending;
                s->pending = p->next;
                fence_put(tl, p->f);
                free(p);
            }
            free(s);
        }
    while (tl->free_fences) {
        tl_fence *f = tl->free_fences;
        tl->free_fences = f->next;
        if (!f->done) tl->wait_fences(tl->dev, 1, &f->fence, VK_TRUE, 1000000000ull);
        tl->destroy_fence(tl->dev, f->fence, NULL);
        free(f);
    }
    pthread_cond_destroy(&tl->cond);
    pthread_mutex_destroy(&tl->lock);
    free(tl);
}

PFN_vkVoidFunction hybris_timeline_dispatch(const char *name)
{
#define PROC(command) if (!strcmp(name, "vk" #command)) return (PFN_vkVoidFunction)tl_##command
#define PROMOTED(command) PROC(command); \
    if (!strcmp(name, "vk" #command "KHR")) return (PFN_vkVoidFunction)tl_##command
    PROC(CreateSemaphore);
    PROC(DestroySemaphore);
    PROMOTED(GetSemaphoreCounterValue);
    PROMOTED(SignalSemaphore);
    PROMOTED(WaitSemaphores);
    PROC(GetSemaphoreFdKHR);
    PROC(ImportSemaphoreFdKHR);
#undef PROMOTED
#undef PROC
    return NULL;
}

int hybris_timeline_host_command(const char *name)
{
    return !strcmp(name, "vkGetSemaphoreCounterValue") || !strcmp(name, "vkGetSemaphoreCounterValueKHR") ||
           !strcmp(name, "vkSignalSemaphore") || !strcmp(name, "vkSignalSemaphoreKHR") ||
           !strcmp(name, "vkWaitSemaphores") || !strcmp(name, "vkWaitSemaphoresKHR");
}

PFN_vkVoidFunction hybris_timeline_submit_proc(const struct hybris_layer_device *device,
    const char *name)
{
    if (!device->timeline) return NULL;
    if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)tl_QueueSubmit;
    if (device->timeline->submit2 && (!strcmp(name, "vkQueueSubmit2") ||
                                       !strcmp(name, "vkQueueSubmit2KHR")))
        return (PFN_vkVoidFunction)tl_QueueSubmit2;
    return NULL;
}
