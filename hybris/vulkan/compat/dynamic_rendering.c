/* SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from MaliBridge server_emu_dynrend.c.
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "dynamic_rendering.h"
#include "../layer/layer.h"
#include "bc_context.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#define MAXC 8
#define MAP_BUCKETS 512
#define DYNAMIC_LOG(...) do { fprintf(stderr,"HYBRIS_DYNAMIC_RENDERING: "); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while (0)
/* What a render pass is made from (zeroed, then filled: compared and hashed as bytes). */
typedef struct att_key {
    uint32_t format, samples, load, store, layout;
    uint32_t res_format, res_layout; /* res_format 0: no resolve */
} att_key;

typedef struct rp_key {
    uint32_t view_mask, ncolor, has_ds;
    att_key color[MAXC];
    uint32_t ds_format, ds_samples, dload, dstore, sload, sstore, dlayout, slayout;
    uint32_t ds_res_format, ds_res_dlayout, ds_res_slayout, dres_mode, sres_mode;
} rp_key;

typedef struct rp_rec {
    rp_key key;
    VkRenderPass rp;
    struct rp_rec *next;
} rp_rec;

typedef struct view_rec {
    VkImageView view;
    VkFormat format;
    VkSampleCountFlagBits samples;
    struct view_rec *next;
} view_rec;

typedef struct hybris_dynamic_state {
    VkDevice device;
    pthread_mutex_t lock;
    rp_rec *rps[MAP_BUCKETS];
    view_rec *views[MAP_BUCKETS];
    uint32_t nrp;
    PFN_vkCreateRenderPass2 create_rp2;
    PFN_vkDestroyRenderPass destroy_rp;
    PFN_vkCreateFramebuffer create_fb;
    PFN_vkDestroyFramebuffer destroy_fb;
    PFN_vkCmdBeginRenderPass begin_rp;
    PFN_vkCmdEndRenderPass end_rp;
    PFN_vkCreateGraphicsPipelines create_gp;
    PFN_vkBeginCommandBuffer begin_cb;
    PFN_vkCreateImageView create_view;
    PFN_vkDestroyImageView destroy_view;
    uint64_t renderings;
} dr_state;


static dr_state *st(const void *handle)
{
    struct hybris_layer_device device;
    return hybris_layer_lookup_device((VkDevice)handle, &device) ? device.dynamic_rendering : NULL;
}
static unsigned hb(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 55) & (MAP_BUCKETS - 1);
}

static unsigned key_hash(const rp_key *k)
{
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(*k); i++) h = (h ^ p[i]) * 1099511628211ull;
    return hb(h);
}

/* ------------------------------------------------------------------ images and views */

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                        VkImageView *out)
{
    dr_state *s = st(device);
    if (!s) { return ((PFN_vkCreateImageView)hybris_layer_device_inner_proc(device,"vkCreateImageView"))(device, ci, a, out); }
    VkResult r = s->create_view(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    view_rec *v = calloc(1, sizeof(*v));
    if (!v) { s->destroy_view(device, *out, a); *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    v->view = *out;
    v->format = ci->format;
    v->samples = VK_SAMPLE_COUNT_1_BIT;
    struct hybris_bc_device *images=hybris_bc_device_find(device);
    struct hybris_bc_resource *image=images?hybris_bc_resource_find(images,ci->image):NULL;
    if(image)v->samples=image->samples;
    pthread_mutex_lock(&s->lock);
    unsigned b = hb((uint64_t)(uintptr_t)*out);
    v->next = s->views[b];
    s->views[b] = v;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL d_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *a)
{
    dr_state *s = st(device);
    if (!s) { ((PFN_vkDestroyImageView)hybris_layer_device_inner_proc(device,"vkDestroyImageView"))(device, view, a); return; }
    if (view) {
        pthread_mutex_lock(&s->lock);
        for (view_rec **pp = &s->views[hb((uint64_t)(uintptr_t)view)]; *pp; pp = &(*pp)->next)
            if ((*pp)->view == view) {
                view_rec *v = *pp;
                *pp = v->next;
                free(v);
                break;
            }
        pthread_mutex_unlock(&s->lock);
    }
    s->destroy_view(device, view, a);
}

/* The view's format and sample count (0: not seen; format UNDEFINED then). */
static VkFormat view_info(dr_state *s, VkImageView view, VkSampleCountFlagBits *samples)
{
    VkFormat f = VK_FORMAT_UNDEFINED;
    *samples = VK_SAMPLE_COUNT_1_BIT;
    pthread_mutex_lock(&s->lock);
    for (view_rec *v = s->views[hb((uint64_t)(uintptr_t)view)]; v; v = v->next)
        if (v->view == view) {
            f = v->format;
            *samples = v->samples;
            break;
        }
    pthread_mutex_unlock(&s->lock);
    return f;
}

/* ------------------------------------------------------------------ render passes */

static int has_depth(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 1;
    default:
        return 0;
    }
}

static int has_stencil(VkFormat f)
{
    return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

/* A layout of one aspect (separate depth/stencil layouts) as the other aspect's counterpart. */
static VkImageLayout stencil_for(VkImageLayout depth)
{
    switch (depth) {
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
    default: return depth;
    }
}

static VkImageLayout depth_for(VkImageLayout stencil)
{
    switch (stencil) {
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    default: return stencil;
    }
}

static int separate_layout(VkImageLayout l)
{
    return l == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
}

static VkRenderPass make_pass(dr_state *s, VkDevice device, const rp_key *k, VkResult *error)
{
    *error=VK_SUCCESS;
    unsigned b = key_hash(k);
    pthread_mutex_lock(&s->lock);
    for (rp_rec *r = s->rps[b]; r; r = r->next)
        if (!memcmp(&r->key, k, sizeof(*k))) {
            VkRenderPass rp = r->rp;
            pthread_mutex_unlock(&s->lock);
            return rp;
        }
    pthread_mutex_unlock(&s->lock);

    VkAttachmentDescription2 att[2 * MAXC + 2];
    VkAttachmentDescriptionStencilLayout att_sl[2];
    VkAttachmentReference2 cref[MAXC], rref[MAXC], dsref, dsres;
    VkAttachmentReferenceStencilLayout ref_sl[2];
    VkSubpassDescriptionDepthStencilResolve dsr;
    uint32_t na = 0;
    int any_res = 0;
    memset(att, 0, sizeof(att));
    for (uint32_t i = 0; i < k->ncolor; i++) {
        const att_key *c = &k->color[i];
        cref[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED, c->layout,
                                           VK_IMAGE_ASPECT_COLOR_BIT};
        rref[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED,
                                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT};
        if (!c->format) continue;
        att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)c->format,
                                             (VkSampleCountFlagBits)c->samples, (VkAttachmentLoadOp)c->load,
                                             (VkAttachmentStoreOp)c->store, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                             VK_ATTACHMENT_STORE_OP_DONT_CARE, (VkImageLayout)c->layout, (VkImageLayout)c->layout};
        cref[i].attachment = na++;
        if (c->res_format) {
            att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)c->res_format,
                                                 VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                                                 VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                 (VkImageLayout)c->res_layout, (VkImageLayout)c->res_layout};
            rref[i].attachment = na++;
            rref[i].layout = (VkImageLayout)c->res_layout;
            any_res = 1;
        }
    }
    VkSubpassDescription2 sp = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, NULL, 0, VK_PIPELINE_BIND_POINT_GRAPHICS, k->view_mask, 0, NULL,
                                k->ncolor, k->ncolor ? cref : NULL, any_res ? rref : NULL, NULL, 0, NULL};
    if (k->has_ds) {
        VkFormat f = (VkFormat)k->ds_format;
        VkImageLayout dl = (VkImageLayout)k->dlayout, sl = (VkImageLayout)k->slayout;
        att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, f, (VkSampleCountFlagBits)k->ds_samples,
                                             (VkAttachmentLoadOp)k->dload, (VkAttachmentStoreOp)k->dstore,
                                             (VkAttachmentLoadOp)k->sload, (VkAttachmentStoreOp)k->sstore, dl, dl};
        VkImageAspectFlags asp = (has_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        dsref = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, na, dl, asp};
        if (has_stencil(f) && has_depth(f) && dl != sl) {
            att_sl[0] = (VkAttachmentDescriptionStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, NULL, sl, sl};
            att[na].pNext = &att_sl[0];
            ref_sl[0] = (VkAttachmentReferenceStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, NULL, sl};
            dsref.pNext = &ref_sl[0];
        }
        na++;
        sp.pDepthStencilAttachment = &dsref;
        if (k->ds_res_format) {
            VkImageLayout rdl = (VkImageLayout)k->ds_res_dlayout, rsl = (VkImageLayout)k->ds_res_slayout;
            att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)k->ds_res_format,
                                                 VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                                                 VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, rdl, rdl};
            dsres = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, na, rdl, asp};
            if (has_stencil(f) && has_depth(f) && rdl != rsl) {
                att_sl[1] = (VkAttachmentDescriptionStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, NULL, rsl, rsl};
                att[na].pNext = &att_sl[1];
                ref_sl[1] = (VkAttachmentReferenceStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, NULL, rsl};
                dsres.pNext = &ref_sl[1];
            }
            na++;
            dsr = (VkSubpassDescriptionDepthStencilResolve){VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE, NULL,
                                                            (VkResolveModeFlagBits)k->dres_mode, (VkResolveModeFlagBits)k->sres_mode,
                                                            &dsres};
            sp.pNext = &dsr;
        }
    }
    VkRenderPassCreateInfo2 ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, 0, na, na ? att : NULL, 1, &sp, 0, NULL, 0, NULL};
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = s->create_rp2(device, &ci, NULL, &rp);
    if (r != VK_SUCCESS) {
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
            DYNAMIC_LOG("dynamic rendering emulation: vkCreateRenderPass2 failed (%d): %u colour, depth/stencil %u (format %u), view mask 0x%x",
                    r, k->ncolor, k->has_ds, k->ds_format, k->view_mask);
        *error=r;
        return VK_NULL_HANDLE;
    }
    rp_rec *n = calloc(1, sizeof(*n));
    if (!n) { s->destroy_rp(device, rp, NULL); *error=VK_ERROR_OUT_OF_HOST_MEMORY; return VK_NULL_HANDLE; }
    n->key = *k;
    n->rp = rp;
    pthread_mutex_lock(&s->lock);
    /* another thread may have made the same one meanwhile: keep the first */
    for (rp_rec *o = s->rps[b]; o; o = o->next)
        if (!memcmp(&o->key, k, sizeof(*k))) {
            VkRenderPass keep = o->rp;
            pthread_mutex_unlock(&s->lock);
            s->destroy_rp(device, rp, NULL);
            free(n);
            return keep;
        }
    n->next = s->rps[b];
    s->rps[b] = n;
    uint32_t total = ++s->nrp;
    pthread_mutex_unlock(&s->lock);
    if (total == 1 || total == 64 || total == 1024)
        DYNAMIC_LOG("dynamic rendering emulation: %u render pass(es) made on device %llu", total, (unsigned long long)(uintptr_t)device);
    return rp;
}

/* A pass compatible with a pipeline's or a secondary's formats (ops and layouts do not count). */
static VkRenderPass compat_pass(dr_state *s, VkDevice device, uint32_t view_mask, uint32_t ncolor, const VkFormat *colors, VkFormat depth,
                                VkFormat stencil, VkSampleCountFlagBits samples, VkResult *error)
{
    rp_key k;
    memset(&k, 0, sizeof(k));
    k.view_mask = view_mask;
    if(ncolor>MAXC){*error=VK_ERROR_FEATURE_NOT_PRESENT;return VK_NULL_HANDLE;}
    k.ncolor = ncolor;
    if (!samples) samples = VK_SAMPLE_COUNT_1_BIT;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        att_key *c = &k.color[i];
        c->format = colors ? colors[i] : VK_FORMAT_UNDEFINED;
        if (!c->format) continue;
        c->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c->samples = samples;
        c->load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        c->store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    VkFormat ds = depth ? depth : stencil;
    if (ds) {
        k.has_ds = 1;
        k.ds_format = ds;
        k.ds_samples = samples;
        k.dload = k.sload = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.dstore = k.sstore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        k.dlayout = k.slayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    return make_pass(s, device, &k, error);
}


struct retained_framebuffer {
    VkDevice device;
    VkFramebuffer framebuffer;
    PFN_vkDestroyFramebuffer destroy;
};
static void release_framebuffer(void *resource)
{
    struct retained_framebuffer *owned=resource;
    owned->destroy(owned->device,owned->framebuffer,NULL);
    free(owned);
}
#define DYNAMIC_LOG_ONCE(...) do { static unsigned count; if (__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)==0) DYNAMIC_LOG(__VA_ARGS__); } while (0)
static VkAttachmentLoadOp load_op(VkAttachmentLoadOp op, int resuming)
{
    return resuming ? VK_ATTACHMENT_LOAD_OP_LOAD : op;
}

static VkAttachmentStoreOp store_op(VkAttachmentStoreOp op, int suspending)
{
    return suspending ? VK_ATTACHMENT_STORE_OP_STORE : op;
}

static VKAPI_ATTR void VKAPI_CALL d_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    dr_state *s = st(cb);
    if (!s) return;
    struct hybris_dynamic_recording *recording = hybris_layer_dynamic_recording(cb);
    if (!recording) return;
    if (ri->colorAttachmentCount > MAXC || ri->pNext) { hybris_layer_command_error(cb, VK_ERROR_FEATURE_NOT_PRESENT); return; }
    int resuming = (ri->flags & VK_RENDERING_RESUMING_BIT) != 0, suspending = (ri->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
    rp_key k;
    memset(&k, 0, sizeof(k));
    k.view_mask = ri->viewMask;
    k.ncolor = ri->colorAttachmentCount;
    VkImageView views[2 * MAXC + 2];
    VkClearValue clears[2 * MAXC + 2];
    uint32_t nv = 0;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        att_key *c = &k.color[i];
        if (!a->imageView) continue;
        if(a->pNext){hybris_layer_command_error(cb,VK_ERROR_FEATURE_NOT_PRESENT);return;}
        c->layout = a->imageLayout;
        VkSampleCountFlagBits sm;
        c->format = view_info(s, a->imageView, &sm);
        if (!c->format) {
            c->layout = 0;
            DYNAMIC_LOG_ONCE("dynamic rendering emulation: colour attachment %u's view %p has no tracked metadata", i,
                     (void *)a->imageView);
            hybris_layer_command_error(cb, VK_ERROR_INITIALIZATION_FAILED); return;
        }
        c->samples = sm;
        c->load = load_op(a->loadOp, resuming);
        c->store = store_op(a->storeOp, suspending);
        clears[nv] = a->clearValue;
        views[nv++] = a->imageView;
        if (a->resolveMode != VK_RESOLVE_MODE_NONE && a->resolveImageView) {
            VkSampleCountFlagBits rs;
            c->res_format = view_info(s, a->resolveImageView, &rs);
            if(!c->res_format){hybris_layer_command_error(cb,VK_ERROR_INITIALIZATION_FAILED);return;}
            {
                c->res_layout = a->resolveImageLayout;
                memset(&clears[nv], 0, sizeof(clears[nv]));
                views[nv++] = a->resolveImageView;
            }
        }
    }
    const VkRenderingAttachmentInfo *da = ri->pDepthAttachment && ri->pDepthAttachment->imageView ? ri->pDepthAttachment : NULL;
    const VkRenderingAttachmentInfo *sa = ri->pStencilAttachment && ri->pStencilAttachment->imageView ? ri->pStencilAttachment : NULL;
    if (da && sa && da->imageView != sa->imageView) {
        DYNAMIC_LOG_ONCE("dynamic rendering emulation: depth and stencil on different views; unsupported by render-pass translation");
        hybris_layer_command_error(cb, VK_ERROR_FEATURE_NOT_PRESENT); return;
    }
    if((da && da->pNext) || (sa && sa->pNext) ||
       (da && sa && da->resolveMode && sa->resolveMode && da->resolveImageView!=sa->resolveImageView)) {
        hybris_layer_command_error(cb,VK_ERROR_FEATURE_NOT_PRESENT);return;
    }
    const VkRenderingAttachmentInfo *dsa = da ? da : sa;
    if (dsa) {
        VkSampleCountFlagBits sm;
        VkFormat f = view_info(s, dsa->imageView, &sm);
        if(!f){hybris_layer_command_error(cb,VK_ERROR_INITIALIZATION_FAILED);return;}
        {
            k.has_ds = 1;
            k.ds_format = f;
            k.ds_samples = sm;
            /* the aspect not attached is kept as it was */
            k.dload = da ? load_op(da->loadOp, resuming) : has_depth(f) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.dstore = da ? store_op(da->storeOp, suspending) : has_depth(f) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            k.sload = sa ? load_op(sa->loadOp, resuming) : has_stencil(f) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.sstore = sa ? store_op(sa->storeOp, suspending) : has_stencil(f) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkImageLayout dl = da ? da->imageLayout : depth_for(sa->imageLayout);
            VkImageLayout sl = sa ? sa->imageLayout : stencil_for(da->imageLayout);
            if (!separate_layout(dl) && !separate_layout(sl)) sl = dl; /* combined layouts: one for both */
            k.dlayout = dl;
            k.slayout = sl;
            VkClearValue cv;
            memset(&cv, 0, sizeof(cv));
            cv.depthStencil.depth = da ? da->clearValue.depthStencil.depth : 0.0f;
            cv.depthStencil.stencil = sa ? sa->clearValue.depthStencil.stencil : 0;
            clears[nv] = cv;
            views[nv++] = dsa->imageView;
            /* depth/stencil resolve: the depth's view and modes, the stencil's mode */
            const VkRenderingAttachmentInfo *ra = da && da->resolveMode && da->resolveImageView ? da
                                                : sa && sa->resolveMode && sa->resolveImageView ? sa : NULL;
            if (ra) {
                VkSampleCountFlagBits rs;
                VkFormat rf = view_info(s, ra->resolveImageView, &rs);
                if(!rf){hybris_layer_command_error(cb,VK_ERROR_INITIALIZATION_FAILED);return;}
                {
                    k.ds_res_format = rf;
                    k.dres_mode = da ? da->resolveMode : VK_RESOLVE_MODE_NONE;
                    k.sres_mode = sa ? sa->resolveMode : VK_RESOLVE_MODE_NONE;
                    VkImageLayout rdl = da && da->resolveImageView ? da->resolveImageLayout : depth_for(sa->resolveImageLayout);
                    VkImageLayout rsl = sa && sa->resolveImageView ? sa->resolveImageLayout : stencil_for(da->resolveImageLayout);
                    if (!separate_layout(rdl) && !separate_layout(rsl)) rsl = rdl;
                    k.ds_res_dlayout = rdl;
                    k.ds_res_slayout = rsl;
                    memset(&clears[nv], 0, sizeof(clears[nv]));
                    views[nv++] = ra->resolveImageView;
                }
            }
        }
    }
    VkResult result;
    VkRenderPass rp = make_pass(s, s->device, &k, &result);
    if (!rp) { hybris_layer_command_error(cb, result); return; }
    uint32_t w = (uint32_t)(ri->renderArea.offset.x > 0 ? ri->renderArea.offset.x : 0) + ri->renderArea.extent.width;
    uint32_t h = (uint32_t)(ri->renderArea.offset.y > 0 ? ri->renderArea.offset.y : 0) + ri->renderArea.extent.height;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, nv ? views : NULL, w ? w : 1, h ? h : 1,
                                   ri->viewMask ? 1 : (ri->layerCount ? ri->layerCount : 1)};
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = s->create_fb(s->device, &fci, NULL, &fb);
    if (r != VK_SUCCESS) {
        DYNAMIC_LOG_ONCE("dynamic rendering emulation: vkCreateFramebuffer failed (%d); command recording failed", r);
        hybris_layer_command_error(cb, r);
        return;
    }
    struct retained_framebuffer *owned = calloc(1, sizeof(*owned));
    if (!owned) { s->destroy_fb(s->device, fb, NULL); hybris_layer_command_error(cb, VK_ERROR_OUT_OF_HOST_MEMORY); return; }
    *owned = (struct retained_framebuffer){.device=s->device, .framebuffer=fb, .destroy=s->destroy_fb};
    r = hybris_layer_command_retain(cb, owned, release_framebuffer);
    if (r != VK_SUCCESS) { release_framebuffer(owned); hybris_layer_command_error(cb, r); return; }
    recording->active = VK_TRUE;
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, ri->renderArea, nv, nv ? clears : NULL};
    s->begin_rp(cb, &bi, (ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                                                                                            : VK_SUBPASS_CONTENTS_INLINE);
    uint64_t nr = __atomic_add_fetch(&s->renderings, 1, __ATOMIC_RELAXED);
    if (nr == 1) DYNAMIC_LOG("dynamic rendering emulation: first vkCmdBeginRendering as a render pass (%u colour, depth/stencil %u)", k.ncolor, k.has_ds);
}

static VKAPI_ATTR void VKAPI_CALL d_CmdEndRendering(VkCommandBuffer cb)
{
    dr_state *s = st(cb);
    struct hybris_dynamic_recording *recording = hybris_layer_dynamic_recording(cb);
    if (s && recording && recording->active) { s->end_rp(cb); recording->active=VK_FALSE; }
}

static const void *find_node(const void *chain, VkStructureType type)
{
    for (const VkBaseInStructure *n=chain;n;n=n->pNext) if(n->sType==type)return n;
    return NULL;
}
static VkResult VKAPI_CALL d_CreateGraphicsPipelines(VkDevice device,VkPipelineCache cache,uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *out)
{
    dr_state *s=st(device);
    PFN_vkCreateGraphicsPipelines next=s?s->create_gp:(PFN_vkCreateGraphicsPipelines)hybris_layer_device_inner_proc(device,"vkCreateGraphicsPipelines");
    if(!s || !count)return next(device,cache,count,infos,allocator,out);
    for(uint32_t i=0;i<count;++i)out[i]=VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo *copies=calloc(count,sizeof(*copies));
    if(!copies)return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(copies,infos,count*sizeof(*copies));
    VkResult result=VK_SUCCESS;
    for(uint32_t i=0;i<count;++i) {
        if(copies[i].renderPass)continue;
        if(copies[i].flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR){result=VK_ERROR_FEATURE_NOT_PRESENT;break;}
        const VkPipelineRenderingCreateInfo *r=find_node(copies[i].pNext,VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        if(r && r->colorAttachmentCount>MAXC){result=VK_ERROR_FEATURE_NOT_PRESENT;break;}
        VkSampleCountFlagBits samples=copies[i].pMultisampleState?copies[i].pMultisampleState->rasterizationSamples:VK_SAMPLE_COUNT_1_BIT;
        copies[i].renderPass=compat_pass(s,device,r?r->viewMask:0,r?r->colorAttachmentCount:0,
            r?r->pColorAttachmentFormats:NULL,r?r->depthAttachmentFormat:VK_FORMAT_UNDEFINED,
            r?r->stencilAttachmentFormat:VK_FORMAT_UNDEFINED,samples,&result);
        copies[i].subpass=0;
        if(!copies[i].renderPass)break;
    }
    if(result==VK_SUCCESS)result=next(device,cache,count,copies,allocator,out);
    else for(uint32_t i=0;i<count;++i)out[i]=VK_NULL_HANDLE;
    free(copies);
    return result;
}
static VkResult VKAPI_CALL d_BeginCommandBuffer(VkCommandBuffer cb,const VkCommandBufferBeginInfo *info)
{
    dr_state *s=st(cb);
    PFN_vkBeginCommandBuffer next=s?s->begin_cb:(PFN_vkBeginCommandBuffer)hybris_layer_handle_inner_proc(cb,"vkBeginCommandBuffer");
    if(!s || !info->pInheritanceInfo || !(info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) || info->pInheritanceInfo->renderPass)
        return next(cb,info);
    const VkCommandBufferInheritanceRenderingInfo *r=find_node(info->pInheritanceInfo->pNext,VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO);
    if(!r)return next(cb,info);
    if(r->colorAttachmentCount>MAXC)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkCommandBufferBeginInfo begin=*info;
    VkCommandBufferInheritanceInfo inheritance=*info->pInheritanceInfo;
    VkResult result;
    inheritance.renderPass=compat_pass(s,s->device,r->viewMask,r->colorAttachmentCount,r->pColorAttachmentFormats,
        r->depthAttachmentFormat,r->stencilAttachmentFormat,r->rasterizationSamples,&result);
    if(!inheritance.renderPass)return result;
    inheritance.subpass=0;inheritance.framebuffer=VK_NULL_HANDLE;
    begin.pInheritanceInfo=&inheritance;
    return next(cb,&begin);
}
int hybris_dynamic_native_extension(VkPhysicalDevice physical,const char *name)
{
    struct hybris_layer_physical instance;
    if(!hybris_layer_lookup_physical(physical,&instance))return 0;
    PFN_vkEnumerateDeviceExtensionProperties query=(PFN_vkEnumerateDeviceExtensionProperties)
        instance.resolver(instance.instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t count=0;
    if(!query || query(physical,NULL,&count,NULL)!=VK_SUCCESS)return 0;
    VkExtensionProperties *extensions=calloc(count?count:1,sizeof(*extensions));
    if(!extensions)return 0;
    int found=0;
    if(query(physical,NULL,&count,extensions)==VK_SUCCESS)
        for(uint32_t i=0;i<count;++i)found|=!strcmp(extensions[i].extensionName,name);
    free(extensions);return found;
}
int hybris_dynamic_emulated(VkPhysicalDevice physical)
{
    if(getauxval(AT_SECURE))return 0;
    const char *option=getenv("HYBRIS_DYNAMIC_RENDERING");
    if(option && !strcmp(option,"0"))return 0;
    struct hybris_layer_physical instance;
    if(!hybris_layer_lookup_physical(physical,&instance))return 0;
    PFN_vkGetPhysicalDeviceProperties query=(PFN_vkGetPhysicalDeviceProperties)
        instance.resolver(instance.instance,"vkGetPhysicalDeviceProperties");
    if(!query)return 0;
    VkPhysicalDeviceProperties properties;query(physical,&properties);
    if(properties.limits.maxColorAttachments>MAXC)return 0;
    int forced=option && !strcmp(option,"force");
    if(!forced && (properties.apiVersion>=VK_API_VERSION_1_3 || hybris_dynamic_native_extension(physical,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)))return 0;
    return (properties.apiVersion>=VK_API_VERSION_1_2 || hybris_dynamic_native_extension(physical,VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) &&
        (properties.apiVersion>=VK_API_VERSION_1_2 || hybris_dynamic_native_extension(physical,VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME));
}
void hybris_dynamic_features(VkPhysicalDevice physical,VkPhysicalDeviceFeatures2 *features)
{
    if(!hybris_dynamic_emulated(physical))return;
    for(VkBaseOutStructure *node=features->pNext;node;node=node->pNext) {
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
            ((VkPhysicalDeviceDynamicRenderingFeatures*)node)->dynamicRendering=VK_TRUE;
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
            ((VkPhysicalDeviceVulkan13Features*)node)->dynamicRendering=VK_TRUE;
    }
}
VkResult hybris_dynamic_attach(struct hybris_layer_device *device,
    PFN_vkVoidFunction (*resolve)(const struct hybris_layer_device *,const char *),const VkDeviceCreateInfo *info)
{
    if(!hybris_dynamic_emulated(device->physical))return VK_SUCCESS;
    const VkPhysicalDeviceDynamicRenderingFeatures *r=find_node(info->pNext,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
    const VkPhysicalDeviceVulkan13Features *v=find_node(info->pNext,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    if(!(r&&r->dynamicRendering) && !(v&&v->dynamicRendering))return VK_SUCCESS;
    dr_state *s=calloc(1,sizeof(*s));
    if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->device=device->handle;
#define LOAD(field,type,name) s->field=(PFN_vk##type)resolve(device,"vk" name)
    LOAD(create_rp2,CreateRenderPass2,"CreateRenderPass2");
    if(!s->create_rp2)LOAD(create_rp2,CreateRenderPass2,"CreateRenderPass2KHR");
    LOAD(destroy_rp,DestroyRenderPass,"DestroyRenderPass");
    LOAD(create_fb,CreateFramebuffer,"CreateFramebuffer");LOAD(destroy_fb,DestroyFramebuffer,"DestroyFramebuffer");
    LOAD(begin_rp,CmdBeginRenderPass,"CmdBeginRenderPass");LOAD(end_rp,CmdEndRenderPass,"CmdEndRenderPass");
    LOAD(create_gp,CreateGraphicsPipelines,"CreateGraphicsPipelines");LOAD(begin_cb,BeginCommandBuffer,"BeginCommandBuffer");
    LOAD(create_view,CreateImageView,"CreateImageView");LOAD(destroy_view,DestroyImageView,"DestroyImageView");
#undef LOAD
    if(!s->create_rp2 || !s->destroy_rp || !s->create_fb || !s->destroy_fb || !s->begin_rp || !s->end_rp || !s->create_gp ||
        !s->begin_cb || !s->create_view || !s->destroy_view) {
        free(s);return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(pthread_mutex_init(&s->lock,NULL)){free(s);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    device->dynamic_rendering=s;
    return VK_SUCCESS;
}
void hybris_dynamic_release(struct hybris_dynamic_state *state)
{
    dr_state *s=state;if(!s)return;
    for(unsigned i=0;i<MAP_BUCKETS;++i) {
        while(s->rps[i]){rp_rec *r=s->rps[i];s->rps[i]=r->next;s->destroy_rp(s->device,r->rp,NULL);free(r);}
        while(s->views[i]){view_rec *v=s->views[i];s->views[i]=v->next;free(v);}
    }
    pthread_mutex_destroy(&s->lock);free(s);
}
PFN_vkVoidFunction hybris_dynamic_dispatch(const char *name)
{
#define PROC(n,f) if(!strcmp(name,"vk" #n))return (PFN_vkVoidFunction)f
    PROC(CreateImageView,d_CreateImageView);PROC(DestroyImageView,d_DestroyImageView);
    PROC(CreateGraphicsPipelines,d_CreateGraphicsPipelines);PROC(BeginCommandBuffer,d_BeginCommandBuffer);
    PROC(CmdBeginRendering,d_CmdBeginRendering);PROC(CmdBeginRenderingKHR,d_CmdBeginRendering);
    PROC(CmdEndRendering,d_CmdEndRendering);PROC(CmdEndRenderingKHR,d_CmdEndRendering);
#undef PROC
    return NULL;
}
PFN_vkVoidFunction hybris_dynamic_proc(const struct hybris_layer_device *device,const char *name)
{return device->dynamic_rendering?hybris_dynamic_dispatch(name):NULL;}
