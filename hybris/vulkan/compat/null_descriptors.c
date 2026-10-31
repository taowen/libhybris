/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Adapted from MaliBridge 95a1e600a51299294e06bb09db2db8cc509fe6a3,
 * server/server_nulldesc.c. This is an independent libhybris integration.
 *
 * Typed, initialized stand-ins keep null descriptor updates away from drivers
 * without native support. Read-only resources and writable sinks are separate.
 * Sink reads after writes are not guaranteed zero: this is a compatibility
 * approximation, not Vulkan robustness2 conformance.
 */
#define VK_NO_PROTOTYPES
#include "null_descriptors.h"
#include "robustness2.h"
#include "../layer/layer.h"
#include "null_map.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#define NULL_LOG(...) do { fprintf(stderr,"HYBRIS_NULL: "); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while(0)
enum { SpvOpTypeInt=21, SpvOpTypeFloat=22, SpvOpTypeArray=28, SpvOpTypeRuntimeArray=29,
       SpvOpTypePointer=32, SpvOpFunction=54, SpvOpVariable=59, SpvOpDecorate=71 };
#define NULL_DEVICE_PROCS(V) \
    V(DestroyDescriptorPool) \
    V(ResetDescriptorPool) \
    V(FreeDescriptorSets) \
    V(AllocateCommandBuffers) \
    V(AllocateDescriptorSets) \
    V(AllocateMemory) \
    V(BeginCommandBuffer) \
    V(BindBufferMemory) \
    V(BindImageMemory) \
    V(CmdBindVertexBuffers) \
    V(CmdBindVertexBuffers2) \
    V(CmdClearColorImage) \
    V(CmdFillBuffer) \
    V(CmdPipelineBarrier) \
    V(CmdPushDescriptorSet) \
    V(CmdPushDescriptorSet2) \
    V(CmdPushDescriptorSetWithTemplate) \
    V(CmdPushDescriptorSetWithTemplate2) \
    V(CreateBuffer) \
    V(CreateBufferView) \
    V(CreateCommandPool) \
    V(CreateComputePipelines) \
    V(CreateDescriptorUpdateTemplate) \
    V(CreateFence) \
    V(CreateGraphicsPipelines) \
    V(CreateImage) \
    V(CreateImageView) \
    V(CreatePipelineLayout) \
    V(CreateShaderModule) \
    V(DestroyBuffer) \
    V(DestroyBufferView) \
    V(DestroyCommandPool) \
    V(DestroyDescriptorSetLayout) \
    V(DestroyDescriptorUpdateTemplate) \
    V(DestroyFence) \
    V(DestroyPipelineLayout) \
    V(DestroyShaderModule) \
    V(EndCommandBuffer) \
    V(FreeCommandBuffers) \
    V(FreeMemory) \
    V(GetBufferMemoryRequirements) \
    V(GetDeviceQueue) \
    V(GetDeviceQueue2) \
    V(GetImageMemoryRequirements) \
    V(QueueSubmit) \
    V(QueueSubmit2) \
    V(UpdateDescriptorSetWithTemplate) \
    V(UpdateDescriptorSets) \
    V(WaitForFences) \
    V(DestroyImage) \
    V(DestroyImageView) \
    V(FlushMappedMemoryRanges) \
    V(MapMemory) \
    V(UnmapMemory)
struct null_dispatch {
#define DECLARE(name) PFN_vk##name vk##name;
    NULL_DEVICE_PROCS(DECLARE)
    DECLARE(GetPhysicalDeviceMemoryProperties)
    DECLARE(GetPhysicalDeviceQueueFamilyProperties)
#undef DECLARE
};

struct null_descriptors;
typedef struct null_device {
    VkDevice device;
    VkPhysicalDevice physical;
    unsigned id;
    void *key;
    struct null_dispatch real, dt;
    struct null_descriptors *nulldesc;
    struct null_device *next;
} null_device;
static pthread_mutex_t devices_guard=PTHREAD_MUTEX_INITIALIZER;
static null_device *devices;
static const char *null_env(const char *name) { return getauxval(AT_SECURE)?NULL:getenv(name); }
static null_device *lookup(const void *handle) {
    void *key=hybris_layer_dispatch_key(handle);
    pthread_mutex_lock(&devices_guard);
    null_device *device=devices;
    while(device && device->key!=key)device=device->next;
    pthread_mutex_unlock(&devices_guard);
    return device;
}
typedef struct nd_template {
    VkDescriptorUpdateTemplate tmpl;
    VkDescriptorSetLayout layout; /* the set layout it writes (push templates: of its pipeline layout's set) */
    uint32_t count;
    VkDescriptorUpdateTemplateEntry *entries;
    struct nd_template *next;
} nd_template;

/* Image stand-in shapes: a view type (VkImageViewType, 1D..cube array) and a numeric class.
 * Kinds number them from 1; 0 is "not known", KIND_MIXED "pipelines disagree". */
enum { CLS_FLOAT, CLS_UINT, CLS_SINT, CLASSES };
#define VIEW_TYPES 7
#define KINDS (VIEW_TYPES * CLASSES)
#define KIND(vt, cls) ((uint8_t)(1 + (vt) * CLASSES + (cls)))
#define KIND_MIXED 0xff
static const VkFormat class_format[CLASSES] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
static const char *const class_name[CLASSES] = {"float", "uint", "sint"};
static const char *const view_name[VIEW_TYPES] = {"1D", "2D", "3D", "cube", "1D array", "2D array", "cube array"};

typedef struct nd_image {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    int tried;
    int cleared; /* cleared to zero and in GENERAL (or being, in a submitted command buffer) */
    uint32_t layers;
} nd_image;

#define MAX_QUEUE_FAMILIES 16

/* Handle maps (nd_map.h): modules, pipeline layouts and set layouts to what is known of them
 * (malloc'd), descriptor sets to their layouts. */

/* A module's image bindings and the kind each wants; a set layout's bindings and theirs. */
typedef struct nd_use {
    uint32_t set, binding;
    uint8_t kind;
} nd_use;

typedef struct nd_uses {
    uint32_t n, cap;
    nd_use u[];
} nd_uses;

typedef struct nd_layout {
    uint32_t n;
    VkDescriptorSetLayout set[];
} nd_layout;


typedef struct null_descriptors {
    null_device *t;
    pthread_mutex_t lock;
    PFN_vkUpdateDescriptorSets update;
    PFN_vkUpdateDescriptorSetWithTemplate update_template;
    PFN_vkCreateDescriptorUpdateTemplate create_template;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_template;
    PFN_vkCmdBindVertexBuffers bind_vb;
    PFN_vkCmdBindVertexBuffers2 bind_vb2;
    PFN_vkCmdPushDescriptorSet push;
    PFN_vkCmdPushDescriptorSet2 push2;
    PFN_vkCmdPushDescriptorSetWithTemplate push_template;
    PFN_vkCmdPushDescriptorSetWithTemplate2 push_template2;
    /* precise stand-ins */
    int precise;
    PFN_vkCreateShaderModule create_module;
    PFN_vkDestroyShaderModule destroy_module;
    PFN_vkCreatePipelineLayout create_layout;
    PFN_vkDestroyPipelineLayout destroy_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_compute;
    PFN_vkAllocateDescriptorSets allocate;
    nd_map modules, layouts, hints, sets, set_pools;
    unsigned long shaped, mixed;
    nd_template *templates;
    int made, failed;
    int lost;
    nd_image shape[2][KINDS]; /* [storage]; KIND(2D, float) is the default stand-in, made with the buffers */
    VkDeviceMemory buffer_mem, sink_mem;
    VkImageView view[2];      /* the default stand-ins' views */
    VkBuffer buffer, sink;    /* read-only nulls (and vertex buffers); storage nulls */
    VkBufferView texel, sink_texel;
    unsigned long replaced;
    /* the clear of new stand-ins, ahead of the next queue submission */
    int pending;              /* atomic: stand-ins not cleared yet */
    int buffers_unzeroed;     /* a buffer could not be zeroed through a mapping: fill it too */
    PFN_vkGetDeviceQueue get_queue;
    PFN_vkGetDeviceQueue2 get_queue2;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    nd_map queues;            /* queue -> family + 1 */
    VkQueueFamilyProperties fam[MAX_QUEUE_FAMILIES];
    uint32_t nfam;
    uint32_t fam_idx[MAX_QUEUE_FAMILIES]; /* 0..nfam-1: the stand-ins are CONCURRENT on all when nfam > 1 */
    VkFence pending_fence;
    VkCommandPool pool[MAX_QUEUE_FAMILIES];
    unsigned long clears;
} null_descriptors;

static null_descriptors *cur(const void *handle) {
    null_device *device=lookup(handle);
    return device?device->nulldesc:NULL;
}

/* A memory type the resource accepts, never lazily allocated (Mali's only GPU-only type is):
 * device-local first, and with `host` host-visible first (device-local and host-visible, then
 * host-visible, then device-local). */
static uint32_t mem_type(null_device *t, uint32_t bits, int host)
{
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    static const VkMemoryPropertyFlags want[2][3] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0},
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
    };
    for (int pass = 0; pass < 4; pass++) {
        VkMemoryPropertyFlags w = pass < 3 ? want[host != 0][pass] : 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) continue;
            if ((f & w) != w) continue;
            return i;
        }
    }
    return UINT32_MAX;
}

/* Fills a host-visible allocation with zeros; 0 when the memory cannot be mapped. */
static int zero_fill(null_device *t, VkDeviceMemory mem, uint32_t type, VkDeviceSize size)
{
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    VkMemoryPropertyFlags f = mp.memoryTypes[type].propertyFlags;
    void *p = NULL;
    if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || t->real.vkMapMemory(t->device, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p)
        return 0;
    memset(p, 0, (size_t)size);
    if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
        t->real.vkFlushMappedMemoryRanges(t->device, 1, &r);
    }
    t->real.vkUnmapMemory(t->device, mem);
    return 1;
}

static void free_image(null_device *t, nd_image *s)
{
    if (s->view) t->real.vkDestroyImageView(t->device, s->view, NULL);
    if (s->image) t->real.vkDestroyImage(t->device, s->image, NULL);
    if (s->mem) t->real.vkFreeMemory(t->device, s->mem, NULL);
    s->view = VK_NULL_HANDLE;
    s->image = VK_NULL_HANDLE;
    s->mem = VK_NULL_HANDLE;
}

/* The stand-ins are used from every queue family the app has: CONCURRENT on all of them when
 * there are several (EXCLUSIVE ones would need an ownership transfer the app does not know of). */
static void set_sharing(const null_descriptors *n, VkSharingMode *mode, uint32_t *count_, const uint32_t **idx)
{
    if (n->nfam < 2) return;
    *mode = VK_SHARING_MODE_CONCURRENT;
    *count_ = n->nfam;
    *idx = n->fam_idx;
}

/* A 1x1 (cubes: 6 faces) sampled and storage image of a kind, and its view; 0 when it could not
 * be made. Cube arrays need imageCubeArray, which a shader declaring one needs too. */
static int make_image(null_descriptors *n, uint8_t kind, nd_image *s)
{
    null_device *t = n->t;
    VkDevice d = t->device;
    const struct null_dispatch *r = &t->real;
    VkImageViewType vt = (VkImageViewType)((kind - 1) / CLASSES);
    VkFormat fmt = class_format[(kind - 1) % CLASSES];
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageType it = vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D
                   : vt == VK_IMAGE_VIEW_TYPE_3D                                     ? VK_IMAGE_TYPE_3D
                                                                                     : VK_IMAGE_TYPE_2D;
    uint32_t layers = cube ? 6 : 1;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, it, fmt, {1, 1, 1},
                             1, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    set_sharing(n, &ici.sharingMode, &ici.queueFamilyIndexCount, &ici.pQueueFamilyIndices);
    VkMemoryRequirements req;
    uint32_t type;
    if (r->vkCreateImage(d, &ici, NULL, &s->image) != VK_SUCCESS) goto fail;
    r->vkGetImageMemoryRequirements(d, s->image, &req);
    if ((type = mem_type(t, req.memoryTypeBits, 0)) == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (r->vkAllocateMemory(d, &ai, NULL, &s->mem) != VK_SUCCESS || r->vkBindImageMemory(d, s->image, s->mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, s->image, vt, fmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    if (r->vkCreateImageView(d, &vci, NULL, &s->view) != VK_SUCCESS) goto fail;
    s->layers = layers;
    s->cleared = 0;
    __atomic_store_n(&n->pending, 1, __ATOMIC_RELEASE);
    return 1;
fail:
    s->view = VK_NULL_HANDLE; /* not made: nothing to destroy */
    free_image(t, s);
    return 0;
}

/* A zero-filled 4 KB buffer for null buffer descriptors and an R32_UINT texel view on it (none
 * when the driver refuses it: texel nulls then stay). 0 when the buffer could not be made. */
static int make_buffer(null_descriptors *n, VkBuffer *buf, VkDeviceMemory *mem, VkBufferView *texel, int *zeroed)
{
    null_device *t = n->t;
    VkDevice d = t->device;
    const struct null_dispatch *r = &t->real;
    VkMemoryRequirements req;
    uint32_t type;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    set_sharing(n, &bci.sharingMode, &bci.queueFamilyIndexCount, &bci.pQueueFamilyIndices);
    if (r->vkCreateBuffer(d, &bci, NULL, buf) != VK_SUCCESS) return 0;
    r->vkGetBufferMemoryRequirements(d, *buf, &req);
    if ((type = mem_type(t, req.memoryTypeBits, 1)) == UINT32_MAX) return 0;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (r->vkAllocateMemory(d, &ai, NULL, mem) != VK_SUCCESS || r->vkBindBufferMemory(d, *buf, *mem, 0) != VK_SUCCESS) return 0;
    *zeroed = zero_fill(t, *mem, type, req.size);
    VkBufferViewCreateInfo bv = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, *buf, VK_FORMAT_R32_UINT, 0, VK_WHOLE_SIZE};
    if (r->vkCreateBufferView(d, &bv, NULL, texel) != VK_SUCCESS) *texel = VK_NULL_HANDLE;
    return 1;
}

/* The dummies, made on first need (under the lock). 0 when they could not be. */
static int make_dummies(null_descriptors *n)
{
    if (n->made) return !n->failed;
    n->made = 1;
    null_device *t = n->t;
    for (int st = 0; st < 2; st++) {
        nd_image *def = &n->shape[st][KIND(VK_IMAGE_VIEW_TYPE_2D, CLS_FLOAT) - 1];
        def->tried = 1;
        if (!make_image(n, KIND(VK_IMAGE_VIEW_TYPE_2D, CLS_FLOAT), def)) goto fail;
        n->view[st] = def->view;
    }
    int z0 = 0, z1 = 0;
    if (!make_buffer(n, &n->buffer, &n->buffer_mem, &n->texel, &z0) || !make_buffer(n, &n->sink, &n->sink_mem, &n->sink_texel, &z1))
        goto fail;
    n->buffers_unzeroed = !z0 || !z1;
    if (n->buffers_unzeroed) __atomic_store_n(&n->pending, 1, __ATOMIC_RELEASE);
    NULL_LOG("device %u: null descriptors emulated (dummy image views, buffers%s; %s; storage nulls have their own)", t->id,
             n->texel ? ", texel buffer views" : "", n->buffers_unzeroed ? "buffers zeroed on the GPU" : "buffers zero-filled");
    return 1;
fail:
    n->failed = 1;
    NULL_LOG("device %u: null descriptor dummies could not be made; device creation fails", t->id);
    return 0;
}

/* The stand-in view of a kind, made on first need (under the lock, dummies made); the default
 * one when the kind is not known or its image could not be made. */
static VkImageView standin(null_descriptors *n, uint8_t kind, int storage)
{
    if (!kind || kind == KIND_MIXED) return n->view[storage];
    nd_image *s = &n->shape[storage][kind - 1];
    if (!s->tried) {
        s->tried = 1;
        const char *vt = view_name[(kind - 1) / CLASSES], *cls = class_name[(kind - 1) % CLASSES];
        if (make_image(n, kind, s)) NULL_LOG("device %u: null descriptor stand-in made: %s, %s%s", n->t->id, vt, cls, storage ? " (storage)" : "");
        else NULL_LOG("device %u: null descriptor stand-in %s, %s could not be made; the 2D one is used", n->t->id, vt, cls);
    }
    if (!s->view) return n->view[storage];
    n->shaped++;
    return s->view;
}

/* ------------------------------------------------------------------ SPIR-V */

#define OP_TYPE_IMAGE 25
#define OP_TYPE_SAMPLED_IMAGE 27
#define DECORATION_BINDING 33
#define DECORATION_DESCRIPTOR_SET 34
#define STORAGE_UNIFORM_CONSTANT 0

enum { T_NONE, T_FLOAT, T_INT, T_IMAGE, T_SAMPLED, T_ARRAY, T_PTR };

typedef struct spv_id {
    uint8_t what, a, b; /* T_INT: a signed, b wider than 32; T_IMAGE: a Dim, b arrayed | multisampled << 1 */
    uint32_t ref;       /* sampled type, image type, element type, pointee */
    uint32_t set, binding; /* decorations + 1 (0: none) */
} spv_id;

/* Adds a use (or a binding's kind, set ignored) to a list; a different kind for the same binding
 * makes it KIND_MIXED. Returns the list (moved, or NULL out of memory: the old one stays). */
static nd_uses *add_use(nd_uses *l, uint32_t set, uint32_t binding, uint8_t kind, unsigned long *mixed)
{
    for (uint32_t i = 0; l && i < l->n; i++)
        if (l->u[i].set == set && l->u[i].binding == binding) {
            if (l->u[i].kind != kind && l->u[i].kind != KIND_MIXED) {
                l->u[i].kind = KIND_MIXED;
                if (mixed) (*mixed)++;
            }
            return l;
        }
    if (!l || l->n == l->cap) {
        uint32_t cap = l ? l->cap * 2 : 8;
        nd_uses *g = realloc(l, sizeof(*g) + cap * sizeof(g->u[0]));
        if (!g) return NULL;
        if (!l) g->n = 0;
        g->cap = cap;
        l = g;
    }
    l->u[l->n++] = (nd_use){set, binding, kind};
    return l;
}

/* The kind of a UniformConstant variable of type ty (a pointer to an image, a sampled image or an
 * array of them); 0 when it is none of those or its shape has no stand-in (buffer and subpass
 * images, multisampled ones, 64-bit integers). */
static uint8_t var_kind(const spv_id *id, uint32_t bound, uint32_t ty)
{
    for (int k = 0; k < 8 && ty < bound && id[ty].what != T_IMAGE; k++) ty = id[ty].ref;
    if (ty >= bound || id[ty].what != T_IMAGE || (id[ty].b & 2)) return 0;
    /* Dim: 1D 2D 3D Cube Rect Buffer SubpassData; not arrayed, arrayed */
    static const int8_t view[2][8] = {
        {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_2D, -1, -1, -1},
        {VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, -1, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, -1, -1, -1, -1}};
    int vt = view[id[ty].b & 1][id[ty].a];
    uint32_t st = id[ty].ref;
    if (vt < 0 || st >= bound) return 0;
    if (id[st].what == T_FLOAT) return KIND(vt, CLS_FLOAT);
    if (id[st].what == T_INT && !id[st].b) return KIND(vt, id[st].a ? CLS_SINT : CLS_UINT);
    return 0;
}

/* The images a module declares (with a set and a binding) and the stand-in kind of each; NULL
 * when none (or the code is not SPIR-V). */
static nd_uses *reflect(const uint32_t *w, size_t bytes)
{
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return NULL;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return NULL;
    spv_id *id = calloc(bound, sizeof(*id));
    if (!id) return NULL;
    nd_uses *l = NULL;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n || op == SpvOpFunction) break; /* declarations all come first */
        const uint32_t *o = &w[i + 1];
        if (len < 3 || o[0] >= bound) continue;
        spv_id *r = &id[o[0]];
        switch (op) {
        case SpvOpDecorate:
            if (len >= 4 && o[1] == DECORATION_DESCRIPTOR_SET) r->set = o[2] + 1;
            if (len >= 4 && o[1] == DECORATION_BINDING) r->binding = o[2] + 1;
            break;
        case SpvOpTypeFloat:
            r->what = T_FLOAT;
            break;
        case SpvOpTypeInt:
            if (len >= 4) *r = (spv_id){T_INT, o[2] != 0, o[1] > 32};
            break;
        case OP_TYPE_IMAGE:
            if (len >= 9) *r = (spv_id){T_IMAGE, (uint8_t)(o[2] < 7 ? o[2] : 7), (uint8_t)((o[4] != 0) | (o[5] != 0) << 1), o[1]};
            break;
        case OP_TYPE_SAMPLED_IMAGE:
            *r = (spv_id){T_SAMPLED, 0, 0, o[1]};
            break;
        case SpvOpTypeArray:
        case SpvOpTypeRuntimeArray:
            *r = (spv_id){T_ARRAY, 0, 0, o[1]};
            break;
        case SpvOpTypePointer:
            if (len >= 4) *r = (spv_id){T_PTR, 0, 0, o[2]};
            break;
        case SpvOpVariable: { /* result type, result, storage class */
            if (len < 4 || o[2] != STORAGE_UNIFORM_CONSTANT || o[1] >= bound) break;
            const spv_id *v = &id[o[1]];
            uint8_t kind = v->set && v->binding ? var_kind(id, bound, o[0]) : 0;
            nd_uses *g = kind ? add_use(l, v->set - 1, v->binding - 1, kind, NULL) : NULL;
            if (g) l = g;
            break;
        }
        }
    }
    free(id);
    return l;
}

/* ------------------------------------------------------------------ what pipelines tell */

/* The set layout a descriptor set was allocated with (the lock is held). */
static VkDescriptorSetLayout set_layout(null_descriptors *n, VkDescriptorSet set)
{
    nd_ent *e = map_find(&n->sets, (uint64_t)(uintptr_t)set);
    return e ? (VkDescriptorSetLayout)(uintptr_t)e->val : VK_NULL_HANDLE;
}

/* Set `set` of a pipeline layout (the lock is held). */
static VkDescriptorSetLayout layout_set(null_descriptors *n, VkPipelineLayout pl, uint32_t set)
{
    nd_ent *e = map_find(&n->layouts, (uint64_t)(uintptr_t)pl);
    nd_layout *l = e ? (nd_layout *)(uintptr_t)e->val : NULL;
    return l && set < l->n ? l->set[set] : VK_NULL_HANDLE;
}

/* The kind a binding of a set layout wants (the lock is held); 0 when not known. */
static uint8_t binding_kind(null_descriptors *n, VkDescriptorSetLayout dsl, uint32_t binding)
{
    if (!n->precise || !dsl) return 0;
    nd_ent *e = map_find(&n->hints, (uint64_t)(uintptr_t)dsl);
    nd_uses *h = e ? (nd_uses *)(uintptr_t)e->val : NULL;
    for (uint32_t i = 0; h && i < h->n; i++)
        if (h->u[i].binding == binding) return h->u[i].kind;
    return 0;
}

/* Notes a pipeline's stages on its layout's set layouts. */
static void learn(null_descriptors *n, VkPipelineLayout pl, uint32_t count, const VkPipelineShaderStageCreateInfo *st)
{
    for (uint32_t s = 0; pl && st && s < count; s++) {
        nd_uses *own = NULL;
        if (!st[s].module)
            for (const VkBaseInStructure *b = st[s].pNext; b; b = b->pNext)
                if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    const VkShaderModuleCreateInfo *mi = (const void *)b;
                    own = reflect(mi->pCode, mi->codeSize);
                }
        pthread_mutex_lock(&n->lock);
        nd_ent *me = st[s].module ? map_find(&n->modules, (uint64_t)(uintptr_t)st[s].module) : NULL;
        const nd_uses *u = me ? (nd_uses *)(uintptr_t)me->val : own;
        for (uint32_t i = 0; u && i < u->n; i++) {
            VkDescriptorSetLayout dsl = layout_set(n, pl, u->u[i].set);
            nd_ent *he = dsl ? map_get(&n->hints, (uint64_t)(uintptr_t)dsl, 1024) : NULL;
            nd_uses *h = he ? add_use((nd_uses *)(uintptr_t)he->val, 0, u->u[i].binding, u->u[i].kind, &n->mixed) : NULL;
            if (h) he->val = (uint64_t)(uintptr_t)h;
        }
        pthread_mutex_unlock(&n->lock);
        free(own);
    }
}

static VkResult VKAPI_CALL nd_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                 VkShaderModule *out)
{
    null_descriptors *n = cur(device);
    VkResult r = n->create_module(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    nd_uses *u = reflect(ci->pCode, ci->codeSize);
    if (!u) return r;
    pthread_mutex_lock(&n->lock);
    nd_ent *e = map_get(&n->modules, (uint64_t)(uintptr_t)*out, 1024);
    if (e) {
        free((void *)(uintptr_t)e->val);
        e->val = (uint64_t)(uintptr_t)u;
    } else {
        free(u);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void VKAPI_CALL nd_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *a)
{
    null_descriptors *n = cur(device);
    pthread_mutex_lock(&n->lock);
    free((void *)(uintptr_t)map_take(&n->modules, (uint64_t)(uintptr_t)m));
    pthread_mutex_unlock(&n->lock);
    n->destroy_module(device, m, a);
}

static VkResult VKAPI_CALL nd_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a,
                                                   VkPipelineLayout *out)
{
    null_descriptors *n = cur(device);
    VkResult r = n->create_layout(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    nd_layout *l = malloc(sizeof(*l) + ci->setLayoutCount * sizeof(l->set[0]));
    if (!l) return r;
    l->n = ci->setLayoutCount;
    if (l->n) memcpy(l->set, ci->pSetLayouts, l->n * sizeof(l->set[0]));
    pthread_mutex_lock(&n->lock);
    nd_ent *e = map_get(&n->layouts, (uint64_t)(uintptr_t)*out, 1024);
    if (e) {
        free((void *)(uintptr_t)e->val);
        e->val = (uint64_t)(uintptr_t)l;
    } else {
        free(l);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void VKAPI_CALL nd_DestroyPipelineLayout(VkDevice device, VkPipelineLayout pl, const VkAllocationCallbacks *a)
{
    null_descriptors *n = cur(device);
    pthread_mutex_lock(&n->lock);
    free((void *)(uintptr_t)map_take(&n->layouts, (uint64_t)(uintptr_t)pl));
    pthread_mutex_unlock(&n->lock);
    n->destroy_layout(device, pl, a);
}

static void VKAPI_CALL nd_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout dsl, const VkAllocationCallbacks *a)
{
    null_descriptors *n = cur(device);
    pthread_mutex_lock(&n->lock);
    free((void *)(uintptr_t)map_take(&n->hints, (uint64_t)(uintptr_t)dsl));
    pthread_mutex_unlock(&n->lock);
    n->destroy_set_layout(device, dsl, a);
}

static VkResult VKAPI_CALL nd_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
    null_descriptors *n = cur(device);
    for (uint32_t i = 0; i < count; i++) learn(n, ci[i].layout, ci[i].stageCount, ci[i].pStages);
    return n->create_gfx(device, cache, count, ci, a, out);
}

static VkResult VKAPI_CALL nd_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                     const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
    null_descriptors *n = cur(device);
    for (uint32_t i = 0; i < count; i++) learn(n, ci[i].layout, 1, &ci[i].stage);
    return n->create_compute(device, cache, count, ci, a, out);
}

static VkResult VKAPI_CALL nd_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    null_descriptors *n = cur(device);
    VkResult r = n->allocate(device, ai, out);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&n->lock);
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        nd_ent *e = map_get(&n->sets, (uint64_t)(uintptr_t)out[i], 8192);
        if (e) e->val = (uint64_t)(uintptr_t)ai->pSetLayouts[i];
        nd_ent *pool=map_get(&n->set_pools,(uint64_t)(uintptr_t)out[i],8192);
        if(pool)pool->val=(uint64_t)(uintptr_t)ai->descriptorPool;
        if(!e || !pool)__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void forget_pool(null_descriptors *n, VkDescriptorPool pool)
{
    pthread_mutex_lock(&n->lock);
    nd_map *m=&n->set_pools;
    for(uint32_t i=0;m->b && i<=m->mask;++i)
        for(nd_ent **slot=&m->b[i];*slot;) {
            nd_ent *entry=*slot;
            if(entry->val==(uint64_t)(uintptr_t)pool) {
                map_take(&n->sets,entry->key);
                *slot=entry->next;free(entry);--m->count;
            } else slot=&entry->next;
        }
    pthread_mutex_unlock(&n->lock);
}
static VkResult VKAPI_CALL nd_FreeDescriptorSets(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet *sets)
{
    null_descriptors *n=cur(device);
    VkResult result=n->t->real.vkFreeDescriptorSets(device,pool,count,sets);
    if(result==VK_SUCCESS) {
        pthread_mutex_lock(&n->lock);
        for(uint32_t i=0;i<count;++i) {
            map_take(&n->sets,(uint64_t)(uintptr_t)sets[i]);
            map_take(&n->set_pools,(uint64_t)(uintptr_t)sets[i]);
        }
        pthread_mutex_unlock(&n->lock);
    }
    return result;
}
static VkResult VKAPI_CALL nd_ResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags)
{
    null_descriptors *n=cur(device);
    VkResult result=n->t->real.vkResetDescriptorPool(device,pool,flags);
    if(result==VK_SUCCESS)forget_pool(n,pool);
    return result;
}
static void VKAPI_CALL nd_DestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks *allocator)
{
    null_descriptors *n=cur(device);
    forget_pool(n,pool);
    n->t->real.vkDestroyDescriptorPool(device,pool,allocator);
}

/* ------------------------------------------------------------------ the fixes */

static int image_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || ty == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           ty == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int buffer_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static int texel_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

/* Fixes one descriptor (of type ty, of a binding wanting kind) at p; 1 when it was null.
 * Dummies must exist; the lock is held. */
static int fix_one(null_descriptors *n, VkDescriptorType ty, uint8_t kind, void *p)
{
    if (image_type(ty)) {
        VkDescriptorImageInfo *ii = p;
        if (ii->imageView) return 0;
        ii->imageView = standin(n, kind, ty == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        return 1;
    }
    if (buffer_type(ty)) {
        VkDescriptorBufferInfo *bi = p;
        if (bi->buffer) return 0;
        bi->buffer = ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ? n->sink : n->buffer;
        bi->offset = 0;
        bi->range = VK_WHOLE_SIZE;
        return 1;
    }
    VkBufferView tv = ty == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ? n->sink_texel : n->texel;
    if (texel_type(ty) && tv) {
        VkBufferView *bv = p;
        if (*bv) return 0;
        *bv = tv;
        return 1;
    }
    return 0;
}

static int need_dummies(null_descriptors *n)
{
    pthread_mutex_lock(&n->lock);
    int ok = make_dummies(n);
    pthread_mutex_unlock(&n->lock);
    return ok;
}

static void count(null_descriptors *n, unsigned k)
{
    if (!k) return;
    unsigned long before = __atomic_fetch_add(&n->replaced, k, __ATOMIC_RELAXED);
    if (!before) NULL_LOG("device %u: first null descriptors replaced by dummies", n->t->id);
}

/* Replaces the null descriptors in a write list; how many there were. Pushed writes name no set:
 * push_layout is their set layout (VK_NULL_HANDLE when not known). */
static unsigned fix_writes(null_descriptors *n, uint32_t nw, const VkWriteDescriptorSet *w, int pushed, VkDescriptorSetLayout push_layout)
{
    unsigned k = 0;
    int ready = -1;
    VkDescriptorSet last = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = push_layout;
    for (uint32_t i = 0; i < nw; i++) {
        VkDescriptorType ty = w[i].descriptorType;
        for (uint32_t j = 0; j < w[i].descriptorCount; j++) {
            void *p = NULL;
            int null = 0;
            if (image_type(ty) && w[i].pImageInfo) {
                p = (void *)&w[i].pImageInfo[j];
                null = !((VkDescriptorImageInfo *)p)->imageView;
            } else if (buffer_type(ty) && w[i].pBufferInfo) {
                p = (void *)&w[i].pBufferInfo[j];
                null = !((VkDescriptorBufferInfo *)p)->buffer;
            } else if (texel_type(ty) && w[i].pTexelBufferView) {
                p = (void *)&w[i].pTexelBufferView[j];
                null = !*(VkBufferView *)p;
            }
            if (!null) continue;
            if (ready < 0) {
                pthread_mutex_lock(&n->lock); /* held to the end: the maps, the stand-ins */
                ready = make_dummies(n);
            }
            if (!ready){__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);continue;}
            if (!pushed && image_type(ty) && w[i].dstSet != last) {
                last = w[i].dstSet;
                dsl = set_layout(n, last);
            }
            k += (unsigned)fix_one(n, ty, image_type(ty) ? binding_kind(n, dsl, w[i].dstBinding) : 0, p);
        }
    }
    if (ready >= 0) pthread_mutex_unlock(&n->lock);
    return k;
}

static void free_writes(uint32_t count_,VkWriteDescriptorSet *writes)
{
    if(!writes)return;
    for(uint32_t i=0;i<count_;++i) {
        if(image_type(writes[i].descriptorType))free((void*)writes[i].pImageInfo);
        else if(buffer_type(writes[i].descriptorType))free((void*)writes[i].pBufferInfo);
        else if(texel_type(writes[i].descriptorType))free((void*)writes[i].pTexelBufferView);
    }
    free(writes);
}
static VkWriteDescriptorSet *copy_writes(null_descriptors *n,uint32_t count_,const VkWriteDescriptorSet *source)
{
    VkWriteDescriptorSet *out=count_?calloc(count_,sizeof(*out)):NULL;
    if(count_ && !out)goto fail;
    for(uint32_t i=0;i<count_;++i) {
        out[i]=source[i];
        size_t size=0;const void *data=NULL;
        if(image_type(out[i].descriptorType)){size=sizeof(VkDescriptorImageInfo);data=source[i].pImageInfo;out[i].pImageInfo=NULL;}
        else if(buffer_type(out[i].descriptorType)){size=sizeof(VkDescriptorBufferInfo);data=source[i].pBufferInfo;out[i].pBufferInfo=NULL;}
        else if(texel_type(out[i].descriptorType)){size=sizeof(VkBufferView);data=source[i].pTexelBufferView;out[i].pTexelBufferView=NULL;}
        if(!size || !data || !out[i].descriptorCount)continue;
        if(out[i].descriptorCount>SIZE_MAX/size)goto fail;
        void *copy=malloc(size*out[i].descriptorCount);
        if(!copy)goto fail;
        memcpy(copy,data,size*out[i].descriptorCount);
        if(image_type(out[i].descriptorType))out[i].pImageInfo=copy;
        else if(buffer_type(out[i].descriptorType))out[i].pBufferInfo=copy;
        else out[i].pTexelBufferView=copy;
    }
    return out;
fail:
    free_writes(count_,out);__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return NULL;
}

static void VKAPI_CALL nd_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                               const VkCopyDescriptorSet *cp)
{
    null_descriptors *n = cur(device);
    VkWriteDescriptorSet *copy=copy_writes(n,nw,w);
    if(nw && !copy)return;
    count(n, fix_writes(n, nw, copy, 0, VK_NULL_HANDLE));
    if(!__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))n->update(device, nw, copy, nc, cp);
    free_writes(nw,copy);
}

static nd_template *find_template(null_descriptors *n, VkDescriptorUpdateTemplate tmpl)
{
    for (nd_template *t = n->templates; t; t = t->next)
        if (t->tmpl == tmpl) return t;
    return NULL;
}

static VkResult VKAPI_CALL nd_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    null_descriptors *n = cur(device);
    VkResult r = n->create_template(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    nd_template *t = calloc(1, sizeof(*t));
    if (!t) {n->destroy_template(device,*out,a);*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
    t->entries = malloc((size_t)ci->descriptorUpdateEntryCount * sizeof(*t->entries) + 1);
    if (!t->entries) {
        free(t);n->destroy_template(device,*out,a);*out=VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if(ci->descriptorUpdateEntryCount)memcpy(t->entries, ci->pDescriptorUpdateEntries, (size_t)ci->descriptorUpdateEntryCount * sizeof(*t->entries));
    t->count = ci->descriptorUpdateEntryCount;
    t->tmpl = *out;
    pthread_mutex_lock(&n->lock);
    t->layout = ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS ? layout_set(n, ci->pipelineLayout, ci->set)
                                                                                        : ci->descriptorSetLayout;
    t->next = n->templates;
    n->templates = t;
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void VKAPI_CALL nd_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks *a)
{
    null_descriptors *n = cur(device);
    pthread_mutex_lock(&n->lock);
    for (nd_template **pp = &n->templates; *pp; pp = &(*pp)->next)
        if ((*pp)->tmpl == tmpl) {
            nd_template *t = *pp;
            *pp = t->next;
            free(t->entries);
            free(t);
            break;
        }
    pthread_mutex_unlock(&n->lock);
    n->destroy_template(device, tmpl, a);
}

/* Replaces the null descriptors in a template's data; how many there were. */
static unsigned fix_template_data(null_descriptors *n, VkDescriptorUpdateTemplate tmpl, const void *data)
{
    unsigned k = 0;
    int ready = -1;
    pthread_mutex_lock(&n->lock);
    nd_template *t = find_template(n, tmpl);
    for (uint32_t i = 0; t && data && i < t->count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        VkDescriptorType ty = e->descriptorType;
        if (!image_type(ty) && !buffer_type(ty) && !texel_type(ty)) continue;
        for (uint32_t j = 0; j < e->descriptorCount; j++) {
            uint8_t *p = (uint8_t *)data + e->offset + (size_t)j * e->stride;
            int null = image_type(ty) ? !((VkDescriptorImageInfo *)p)->imageView
                     : buffer_type(ty) ? !((VkDescriptorBufferInfo *)p)->buffer
                                       : !*(VkBufferView *)p;
            if (!null) continue;
            if (ready < 0) ready = make_dummies(n); /* the lock is held */
            if (ready) k += (unsigned)fix_one(n, ty, image_type(ty) ? binding_kind(n, t->layout, e->dstBinding) : 0, p);
        }
    }
    pthread_mutex_unlock(&n->lock);
    return k;
}

static size_t template_element_size(VkDescriptorType type)
{
    if(image_type(type) || type==VK_DESCRIPTOR_TYPE_SAMPLER)return sizeof(VkDescriptorImageInfo);
    if(buffer_type(type))return sizeof(VkDescriptorBufferInfo);
    if(texel_type(type))return sizeof(VkBufferView);
    if(type==VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)return 1;
    if(type==VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)return sizeof(VkAccelerationStructureKHR);
    return 0;
}
static void *copy_template(null_descriptors *n,VkDescriptorUpdateTemplate handle,const void *data)
{
    pthread_mutex_lock(&n->lock);
    nd_template *t=find_template(n,handle);
    size_t bytes=0;
    for(uint32_t i=0;t && i<t->count;++i) {
        const VkDescriptorUpdateTemplateEntry *e=&t->entries[i];
        size_t size=template_element_size(e->descriptorType),stride=e->descriptorType==VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK?1:e->stride;
        if(!e->descriptorCount)continue;
        if(!size || e->offset>SIZE_MAX-size || (e->descriptorCount>1 && stride>(SIZE_MAX-e->offset-size)/(e->descriptorCount-1)))goto fail;
        size_t end=e->offset+(size_t)(e->descriptorCount-1)*stride+size;
        if(end>bytes)bytes=end;
    }
    if(!t || (bytes && !data))goto fail;
    if(!bytes)bytes=1;
    uint8_t *copy=calloc(1,bytes);
    if(!copy)goto fail;
    for(uint32_t i=0;i<t->count;++i) {
        const VkDescriptorUpdateTemplateEntry *e=&t->entries[i];
        size_t size=template_element_size(e->descriptorType);
        for(uint32_t j=0;j<e->descriptorCount;++j) {
            size_t stride=e->descriptorType==VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK?1:e->stride;
            size_t offset=e->offset+(size_t)j*stride;
            memcpy(copy+offset,(const uint8_t*)data+offset,size);
        }
    }
    pthread_mutex_unlock(&n->lock);return copy;
fail:
    pthread_mutex_unlock(&n->lock);__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return NULL;
}
static void VKAPI_CALL nd_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    null_descriptors *n = cur(device);
    void *copy=copy_template(n,tmpl,data);
    if(!copy)return;
    count(n, fix_template_data(n, tmpl, copy));
    if(!__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))n->update_template(device, set, tmpl, copy);
    free(copy);
}

/* The set layout of a pushed set. */
static VkDescriptorSetLayout push_layout(null_descriptors *n, VkPipelineLayout pl, uint32_t set)
{
    if (!n->precise) return VK_NULL_HANDLE;
    pthread_mutex_lock(&n->lock);
    VkDescriptorSetLayout dsl = layout_set(n, pl, set);
    pthread_mutex_unlock(&n->lock);
    return dsl;
}

static void count_push(null_descriptors *n, unsigned k)
{
    static int told;
    if (k && !__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
        NULL_LOG("device %u: null descriptors in push descriptor writes replaced by dummies", n->t->id);
    count(n, k);
}

static void VKAPI_CALL nd_CmdPushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                               uint32_t nw, const VkWriteDescriptorSet *w)
{
    null_descriptors *n = cur(cb);
    VkWriteDescriptorSet *copy=copy_writes(n,nw,w);
    if(nw && !copy)return;
    count_push(n, fix_writes(n, nw, copy, 1, push_layout(n, layout, set)));
    if(!__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))n->push(cb, bp, layout, set, nw, copy);
    free_writes(nw,copy);
}

static void VKAPI_CALL nd_CmdPushDescriptorSet2(VkCommandBuffer cb, const VkPushDescriptorSetInfo *info)
{
    null_descriptors *n = cur(cb);
    VkWriteDescriptorSet *writes=copy_writes(n,info->descriptorWriteCount,info->pDescriptorWrites);
    if(info->descriptorWriteCount && !writes)return;
    count_push(n, fix_writes(n, info->descriptorWriteCount, writes, 1, push_layout(n, info->layout, info->set)));
    VkPushDescriptorSetInfo copy=*info;copy.pDescriptorWrites=writes;
    if(!__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))n->push2(cb, &copy);
    free_writes(info->descriptorWriteCount,writes);
}

static void VKAPI_CALL nd_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                           uint32_t set, const void *data)
{
    null_descriptors *n = cur(cb);
    void *copy=copy_template(n,tmpl,data);
    if(!copy)return;
    count_push(n, fix_template_data(n, tmpl, copy));
    if(!__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))n->push_template(cb, tmpl, layout, set, copy);
    free(copy);
}

static void VKAPI_CALL nd_CmdPushDescriptorSetWithTemplate2(VkCommandBuffer cb,const VkPushDescriptorSetWithTemplateInfo *info)
{
    null_descriptors *n=cur(cb);
    void *data=copy_template(n,info->descriptorUpdateTemplate,info->pData);
    if(!data)return;
    count_push(n,fix_template_data(n,info->descriptorUpdateTemplate,data));
    VkPushDescriptorSetWithTemplateInfo copy=*info;copy.pData=data;
    if(!__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))n->push_template2(cb,&copy);
    free(data);
}

static void VKAPI_CALL nd_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                               const VkDeviceSize *offs)
{
    null_descriptors *n = cur(cb);
    int null=0;
    for(uint32_t i=0;i<count_;++i)null|=bufs[i]==VK_NULL_HANDLE;
    if(!null){n->bind_vb(cb,first,count_,bufs,offs);return;}
    VkBuffer *buffers=malloc((size_t)count_*(sizeof(*buffers)+sizeof(VkDeviceSize)));
    if(!buffers){__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return;}
    VkDeviceSize *offsets=(void*)(buffers+count_);
    memcpy(buffers,bufs,count_*sizeof(*buffers));memcpy(offsets,offs,count_*sizeof(*offsets));
    for (uint32_t i = 0; i < count_; i++)
        if (!bufs[i] && need_dummies(n)) {
            buffers[i] = n->buffer;
            offsets[i] = 0;
        }
    n->bind_vb(cb, first, count_, buffers, offsets);free(buffers);
}

static void VKAPI_CALL nd_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                                const VkDeviceSize *offs, const VkDeviceSize *sizes, const VkDeviceSize *strides)
{
    null_descriptors *n = cur(cb);
    int null=0;
    for(uint32_t i=0;i<count_;++i)null|=bufs[i]==VK_NULL_HANDLE;
    if(!null){n->bind_vb2(cb,first,count_,bufs,offs,sizes,strides);return;}
    VkBuffer *buffers=malloc((size_t)count_*(sizeof(*buffers)+2*sizeof(VkDeviceSize)));
    if(!buffers){__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return;}
    VkDeviceSize *offsets=(void*)(buffers+count_),*ranges=offsets+count_;
    memcpy(buffers,bufs,count_*sizeof(*buffers));memcpy(offsets,offs,count_*sizeof(*offsets));
    if(sizes)memcpy(ranges,sizes,count_*sizeof(*ranges));
    for (uint32_t i = 0; i < count_; i++)
        if (!bufs[i] && need_dummies(n)) {
            buffers[i] = n->buffer;
            offsets[i] = 0;
            if (sizes) ranges[i] = VK_WHOLE_SIZE;
        }
    n->bind_vb2(cb, first, count_, buffers, offsets, sizes?ranges:NULL, strides);free(buffers);
}

/* ------------------------------------------------------------------ clearing the stand-ins */

static void note_queue(null_descriptors *n, VkQueue q, uint32_t family)
{
    if (!q) return;
    pthread_mutex_lock(&n->lock);
    nd_ent *e = map_get(&n->queues, (uint64_t)(uintptr_t)q, 64);
    if (e) e->val = (uint64_t)family + 1;
    pthread_mutex_unlock(&n->lock);
}

static void VKAPI_CALL nd_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *q)
{
    null_descriptors *n = cur(device);
    n->get_queue(device, family, index, q);
    note_queue(n, *q, family);
}

static void VKAPI_CALL nd_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
    null_descriptors *n = cur(device);
    n->get_queue2(device, info, q);
    note_queue(n, *q, info->queueFamilyIndex);
}

/* Records and submits, on q ahead of the app's submission, the clear of every stand-in not cleared
 * yet: UNDEFINED -> TRANSFER_DST, zero, -> GENERAL, and a barrier that makes the writes available;
 * then waits for it on a fence, so that a submission on any queue after this one finds them
 * cleared (another thread's submission waits on the lock meanwhile). It goes down the chain like
 * the app's own (n->submit). Under the lock.
 * 1 when nothing is left to clear, 0 when q cannot clear (the next submission elsewhere will). */
static int clear_pending(null_descriptors *n, VkQueue q)
{
    null_device *t = n->t;
    VkDevice d = t->device;
    const struct null_dispatch *r = &t->real;
    nd_ent *e = map_find(&n->queues, (uint64_t)(uintptr_t)q);
    if (!e || !e->val) return 0;
    uint32_t fam = (uint32_t)(e->val - 1);
    if (fam >= n->nfam || fam >= MAX_QUEUE_FAMILIES || !(n->fam[fam].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
        return 0; /* a transfer-only queue cannot clear colour images: the next submission elsewhere will */
    if (!n->pool[fam]) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam};
        if (r->vkCreateCommandPool(d, &pci, NULL, &n->pool[fam]) != VK_SUCCESS) {
            n->pool[fam] = VK_NULL_HANDLE;
            __atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return 0;
        }
    }
    /* what a barrier may name on this family (VUID-vkCmdPipelineBarrier-dstAccessMask-02816) */
    VkAccessFlags reads = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    if (n->fam[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT) reads |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, n->pool[fam], VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (r->vkAllocateCommandBuffers(d, &cai, &cb) != VK_SUCCESS) {__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return 0;}
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if(r->vkBeginCommandBuffer(cb,&bi)!=VK_SUCCESS){__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return 0;}
    unsigned images = 0;
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < KINDS; i++) {
            nd_image *s = &n->shape[st][i];
            if (!s->image || s->cleared) continue;
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s->layers};
            VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED, s->image, range};
            r->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            VkClearColorValue zero = {{0}};
            r->vkCmdClearColorImage(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            r->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            s->cleared = 1;
            images++;
        }
    if (n->buffers_unzeroed) {
        r->vkCmdFillBuffer(cb, n->buffer, 0, VK_WHOLE_SIZE, 0);
        r->vkCmdFillBuffer(cb, n->sink, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, reads};
        r->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        n->buffers_unzeroed = 0;
    }
    if(r->vkEndCommandBuffer(cb)!=VK_SUCCESS){__atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);return 0;}
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkFence fence = VK_NULL_HANDLE;
    VkResult res = r->vkCreateFence(d, &fci, NULL, &fence);
    if (res == VK_SUCCESS && n->submit) {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        res = n->submit(q, 1, &si, fence);
    } else if (res == VK_SUCCESS) {
        VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
        VkSubmitInfo2 si2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 0, NULL};
        res = n->submit2(q, 1, &si2, fence);
    }
    if (res == VK_SUCCESS) {
        res = r->vkWaitForFences(d, 1, &fence, VK_TRUE, 5000000000ull);
        if (res == VK_TIMEOUT) {
            /* still running: the fence and command buffer stay (the pool frees the latter) */
            NULL_LOG("device %u: the clear of the null descriptor stand-ins did not finish in 5 s; rejecting application submissions", t->id);
            n->pending_fence=fence;
            __atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);
            return 0;
        }
    }
    if (fence) r->vkDestroyFence(d, fence, NULL);
    if (res == VK_SUCCESS) r->vkFreeCommandBuffers(d, n->pool[fam], 1, &cb);
    if (res != VK_SUCCESS) {
        NULL_LOG("device %u: clearing the null descriptor stand-ins failed (%d)", t->id, res);
        __atomic_store_n(&n->lost,1,__ATOMIC_RELEASE);
        return 0;
    }
    if (!n->clears++ || images)
        NULL_LOG("device %u: %u null descriptor stand-in image(s) cleared to zero ahead of a submission (family %u)", t->id, images, fam);
    return 1;
}

static void clear_before_submit(null_descriptors *n, VkQueue q)
{
    if (!n || !__atomic_load_n(&n->pending, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&n->lock);
    if (__atomic_load_n(&n->pending, __ATOMIC_ACQUIRE)) {
        if (clear_pending(n, q)) {
            __atomic_store_n(&n->pending, 0, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&n->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL nd_QueueSubmit(VkQueue q, uint32_t count_, const VkSubmitInfo *s, VkFence f)
{
    null_descriptors *n = cur(q);
    if(__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))return VK_ERROR_DEVICE_LOST;
    clear_before_submit(n, q);
    if(__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))return VK_ERROR_DEVICE_LOST;
    return n->submit(q, count_, s, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL nd_QueueSubmit2(VkQueue q, uint32_t count_, const VkSubmitInfo2 *s, VkFence f)
{
    null_descriptors *n = cur(q);
    if(__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))return VK_ERROR_DEVICE_LOST;
    clear_before_submit(n, q);
    if(__atomic_load_n(&n->lost,__ATOMIC_ACQUIRE))return VK_ERROR_DEVICE_LOST;
    return n->submit2(q, count_, s, f);
}

static void initialize(null_device *t)
{
    struct null_dispatch *dt = &t->dt;
    if (!dt->vkUpdateDescriptorSets) return;
    null_descriptors *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->t = t;
    pthread_mutex_init(&n->lock, NULL);
    n->update = dt->vkUpdateDescriptorSets;
    n->update_template = dt->vkUpdateDescriptorSetWithTemplate;
    n->create_template = dt->vkCreateDescriptorUpdateTemplate;
    n->destroy_template = dt->vkDestroyDescriptorUpdateTemplate;
    n->bind_vb = dt->vkCmdBindVertexBuffers;
    n->bind_vb2 = dt->vkCmdBindVertexBuffers2;
    dt->vkUpdateDescriptorSets = nd_UpdateDescriptorSets;
    if (n->update_template && n->create_template && n->destroy_template) {
        dt->vkUpdateDescriptorSetWithTemplate = nd_UpdateDescriptorSetWithTemplate;
        dt->vkCreateDescriptorUpdateTemplate = nd_CreateDescriptorUpdateTemplate;
        dt->vkDestroyDescriptorUpdateTemplate = nd_DestroyDescriptorUpdateTemplate;
    }
    if (n->bind_vb) dt->vkCmdBindVertexBuffers = nd_CmdBindVertexBuffers;
    if (n->bind_vb2) dt->vkCmdBindVertexBuffers2 = nd_CmdBindVertexBuffers2;
    n->push = dt->vkCmdPushDescriptorSet;
    n->push2 = dt->vkCmdPushDescriptorSet2;
    n->push_template = dt->vkCmdPushDescriptorSetWithTemplate;
    n->push_template2 = dt->vkCmdPushDescriptorSetWithTemplate2;
    if (n->push) dt->vkCmdPushDescriptorSet = nd_CmdPushDescriptorSet;
    if (n->push2) dt->vkCmdPushDescriptorSet2 = nd_CmdPushDescriptorSet2;
    /* Template data is read through the templates this layer saw created. */
    if (n->push_template && dt->vkCreateDescriptorUpdateTemplate == nd_CreateDescriptorUpdateTemplate)
        dt->vkCmdPushDescriptorSetWithTemplate = nd_CmdPushDescriptorSetWithTemplate;
    if(n->push_template2 && dt->vkCreateDescriptorUpdateTemplate==nd_CreateDescriptorUpdateTemplate)
        dt->vkCmdPushDescriptorSetWithTemplate2=nd_CmdPushDescriptorSetWithTemplate2;
    /* Precise stand-ins: what pipelines declare, and which layout each set has. */
    const char *two_d = null_env("HYBRIS_NULLDESC_2D");
    n->create_module = dt->vkCreateShaderModule;
    n->destroy_module = dt->vkDestroyShaderModule;
    n->create_layout = dt->vkCreatePipelineLayout;
    n->destroy_layout = dt->vkDestroyPipelineLayout;
    n->destroy_set_layout = dt->vkDestroyDescriptorSetLayout;
    n->create_gfx = dt->vkCreateGraphicsPipelines;
    n->create_compute = dt->vkCreateComputePipelines;
    n->allocate = dt->vkAllocateDescriptorSets;
    n->precise = !(two_d && *two_d && strcmp(two_d, "0")) && n->create_module && n->destroy_module && n->create_layout &&
                 n->destroy_layout && n->destroy_set_layout && n->create_gfx && n->create_compute && n->allocate;
    if (n->precise) {
        dt->vkCreateShaderModule = nd_CreateShaderModule;
        dt->vkDestroyShaderModule = nd_DestroyShaderModule;
        dt->vkCreatePipelineLayout = nd_CreatePipelineLayout;
        dt->vkDestroyPipelineLayout = nd_DestroyPipelineLayout;
        dt->vkDestroyDescriptorSetLayout = nd_DestroyDescriptorSetLayout;
        dt->vkCreateGraphicsPipelines = nd_CreateGraphicsPipelines;
        dt->vkCreateComputePipelines = nd_CreateComputePipelines;
        dt->vkAllocateDescriptorSets = nd_AllocateDescriptorSets;
        dt->vkDestroyDescriptorPool = nd_DestroyDescriptorPool;
        dt->vkResetDescriptorPool = nd_ResetDescriptorPool;
        dt->vkFreeDescriptorSets = nd_FreeDescriptorSets;
    } else {
        NULL_LOG("device %u: null images all get the 2D stand-in%s", t->id, two_d ? " (HYBRIS_NULLDESC_2D)" : "");
    }
    /* the clear of the stand-ins rides on the app's submissions */
    t->real.vkGetPhysicalDeviceQueueFamilyProperties(t->physical, &n->nfam, NULL);
    if (n->nfam > MAX_QUEUE_FAMILIES) n->nfam = MAX_QUEUE_FAMILIES;
    t->real.vkGetPhysicalDeviceQueueFamilyProperties(t->physical, &n->nfam, n->fam);
    for (uint32_t i = 0; i < n->nfam; i++) n->fam_idx[i] = i;
    n->get_queue = dt->vkGetDeviceQueue;
    n->get_queue2 = dt->vkGetDeviceQueue2;
    n->submit = dt->vkQueueSubmit;
    n->submit2 = dt->vkQueueSubmit2;
    if (n->get_queue) dt->vkGetDeviceQueue = nd_GetDeviceQueue;
    if (n->get_queue2) dt->vkGetDeviceQueue2 = nd_GetDeviceQueue2;
    if (n->submit) dt->vkQueueSubmit = nd_QueueSubmit;
    if (n->submit2) dt->vkQueueSubmit2 = nd_QueueSubmit2;
    t->nulldesc = n;
}

static void destroy(null_device *t)
{
    null_descriptors *n = t->nulldesc;
    if (!n) return;
    const struct null_dispatch *r = &t->real;
    VkDevice d = t->device;
    /* every clear was waited for when it was submitted (or timed out: then the app's own wait
     * for idle before vkDestroyDevice covers it) */
    if(n->pending_fence)r->vkDestroyFence(d,n->pending_fence,NULL);
    for (uint32_t i = 0; i < MAX_QUEUE_FAMILIES; i++)
        if (n->pool[i]) r->vkDestroyCommandPool(d, n->pool[i], NULL);
    if (n->texel) r->vkDestroyBufferView(d, n->texel, NULL);
    if (n->buffer) r->vkDestroyBuffer(d, n->buffer, NULL);
    if (n->buffer_mem) r->vkFreeMemory(d, n->buffer_mem, NULL);
    if (n->sink_texel) r->vkDestroyBufferView(d, n->sink_texel, NULL);
    if (n->sink) r->vkDestroyBuffer(d, n->sink, NULL);
    if (n->sink_mem) r->vkFreeMemory(d, n->sink_mem, NULL);
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < KINDS; i++) free_image(t, &n->shape[st][i]);
    if (n->clears) NULL_LOG("device %u: null descriptor stand-ins cleared to zero in %lu submission(s)", t->id, n->clears);
    if (n->replaced)
        NULL_LOG("device %u: %lu null descriptors replaced, %lu images by stand-ins of their binding's shape; %lu bindings where pipelines "
                 "disagree on the shape", t->id, n->replaced, n->shaped, n->mixed);
    map_free(&n->modules, 1);
    map_free(&n->layouts, 1);
    map_free(&n->hints, 1);
    map_free(&n->sets, 0);
    map_free(&n->set_pools, 0);
    map_free(&n->queues, 0);
    while (n->templates) {
        nd_template *x = n->templates;
        n->templates = x->next;
        free(x->entries);
        free(x);
    }
    pthread_mutex_destroy(&n->lock);
    free(n);
    t->nulldesc = NULL;
}

VkResult hybris_null_attach(const struct hybris_layer_device *owner,
    PFN_vkVoidFunction (*base)(const struct hybris_layer_device *,const char *),
    const VkDeviceCreateInfo *requested) {
    struct hybris_robustness2_policy policy;
    VkResult result=hybris_robustness2_policy(owner->physical,&policy);
    if(result!=VK_SUCCESS)return result;
    int enabled=0;
    for(const VkBaseInStructure *node=requested->pNext;node;node=node->pNext)
        if(node->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT)
            enabled=((const VkPhysicalDeviceRobustness2FeaturesEXT *)node)->nullDescriptor;
    if(!(policy.emulated & HYBRIS_ROBUST_NULL) || !enabled)return VK_SUCCESS;
    null_device *device=calloc(1,sizeof(*device));
    if(!device)return VK_ERROR_OUT_OF_HOST_MEMORY;
    device->device=owner->handle;device->physical=owner->physical;
    device->id=(unsigned)owner->generation;device->key=hybris_layer_dispatch_key(owner->handle);
    struct hybris_layer_physical instance;
    if(!hybris_layer_lookup_physical(owner->physical,&instance)){free(device);return VK_ERROR_INITIALIZATION_FAILED;}
#define LOAD(name) do { \
    device->real.vk##name=(PFN_vk##name)owner->resolver(owner->handle,"vk" #name); \
    device->dt.vk##name=(PFN_vk##name)base(owner,"vk" #name); \
} while(0);
    NULL_DEVICE_PROCS(LOAD)
#undef LOAD
    if(!device->real.vkCmdBindVertexBuffers2)device->real.vkCmdBindVertexBuffers2=(PFN_vkCmdBindVertexBuffers2)owner->resolver(owner->handle,"vkCmdBindVertexBuffers2EXT");
    if(!device->dt.vkCmdBindVertexBuffers2)device->dt.vkCmdBindVertexBuffers2=(PFN_vkCmdBindVertexBuffers2)base(owner,"vkCmdBindVertexBuffers2EXT");
    if(!device->real.vkCmdPushDescriptorSet)device->real.vkCmdPushDescriptorSet=(PFN_vkCmdPushDescriptorSet)owner->resolver(owner->handle,"vkCmdPushDescriptorSetKHR");
    if(!device->dt.vkCmdPushDescriptorSet)device->dt.vkCmdPushDescriptorSet=(PFN_vkCmdPushDescriptorSet)base(owner,"vkCmdPushDescriptorSetKHR");
    if(!device->real.vkCmdPushDescriptorSet2)device->real.vkCmdPushDescriptorSet2=(PFN_vkCmdPushDescriptorSet2)owner->resolver(owner->handle,"vkCmdPushDescriptorSet2KHR");
    if(!device->dt.vkCmdPushDescriptorSet2)device->dt.vkCmdPushDescriptorSet2=(PFN_vkCmdPushDescriptorSet2)base(owner,"vkCmdPushDescriptorSet2KHR");
    if(!device->real.vkCmdPushDescriptorSetWithTemplate)device->real.vkCmdPushDescriptorSetWithTemplate=(PFN_vkCmdPushDescriptorSetWithTemplate)owner->resolver(owner->handle,"vkCmdPushDescriptorSetWithTemplateKHR");
    if(!device->dt.vkCmdPushDescriptorSetWithTemplate)device->dt.vkCmdPushDescriptorSetWithTemplate=(PFN_vkCmdPushDescriptorSetWithTemplate)base(owner,"vkCmdPushDescriptorSetWithTemplateKHR");
    if(!device->real.vkCreateDescriptorUpdateTemplate)device->real.vkCreateDescriptorUpdateTemplate=(PFN_vkCreateDescriptorUpdateTemplate)owner->resolver(owner->handle,"vkCreateDescriptorUpdateTemplateKHR");
    if(!device->dt.vkCreateDescriptorUpdateTemplate)device->dt.vkCreateDescriptorUpdateTemplate=(PFN_vkCreateDescriptorUpdateTemplate)base(owner,"vkCreateDescriptorUpdateTemplateKHR");
    if(!device->real.vkDestroyDescriptorUpdateTemplate)device->real.vkDestroyDescriptorUpdateTemplate=(PFN_vkDestroyDescriptorUpdateTemplate)owner->resolver(owner->handle,"vkDestroyDescriptorUpdateTemplateKHR");
    if(!device->dt.vkDestroyDescriptorUpdateTemplate)device->dt.vkDestroyDescriptorUpdateTemplate=(PFN_vkDestroyDescriptorUpdateTemplate)base(owner,"vkDestroyDescriptorUpdateTemplateKHR");
    if(!device->real.vkUpdateDescriptorSetWithTemplate)device->real.vkUpdateDescriptorSetWithTemplate=(PFN_vkUpdateDescriptorSetWithTemplate)owner->resolver(owner->handle,"vkUpdateDescriptorSetWithTemplateKHR");
    if(!device->dt.vkUpdateDescriptorSetWithTemplate)device->dt.vkUpdateDescriptorSetWithTemplate=(PFN_vkUpdateDescriptorSetWithTemplate)base(owner,"vkUpdateDescriptorSetWithTemplateKHR");
    if(!device->real.vkQueueSubmit2)device->real.vkQueueSubmit2=(PFN_vkQueueSubmit2)owner->resolver(owner->handle,"vkQueueSubmit2KHR");
    if(!device->dt.vkQueueSubmit2)device->dt.vkQueueSubmit2=(PFN_vkQueueSubmit2)base(owner,"vkQueueSubmit2KHR");
    device->real.vkGetPhysicalDeviceQueueFamilyProperties=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)instance.resolver(instance.instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    device->real.vkGetPhysicalDeviceMemoryProperties=(PFN_vkGetPhysicalDeviceMemoryProperties)instance.resolver(instance.instance,"vkGetPhysicalDeviceMemoryProperties");
    if(!device->dt.vkCmdPushDescriptorSetWithTemplate2)device->dt.vkCmdPushDescriptorSetWithTemplate2=(PFN_vkCmdPushDescriptorSetWithTemplate2)base(owner,"vkCmdPushDescriptorSetWithTemplate2KHR");
    initialize(device);
    if(!device->nulldesc){free(device);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    if(!need_dummies(device->nulldesc)){destroy(device);free(device);return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    pthread_mutex_lock(&devices_guard);device->next=devices;devices=device;pthread_mutex_unlock(&devices_guard);
    return VK_SUCCESS;
}
void hybris_null_release(VkDevice handle) {
    pthread_mutex_lock(&devices_guard);
    null_device *device=NULL;
    for(null_device **slot=&devices;*slot;slot=&(*slot)->next)
        if((*slot)->device==handle){device=*slot;*slot=device->next;break;}
    pthread_mutex_unlock(&devices_guard);
    if(device){destroy(device);free(device);}
}
PFN_vkVoidFunction hybris_null_proc(VkDevice handle,const char *name) {
    if(!strcmp(name,"vkCmdBindVertexBuffers2EXT"))name="vkCmdBindVertexBuffers2";
    if(!strcmp(name,"vkCmdPushDescriptorSetKHR"))name="vkCmdPushDescriptorSet";
    if(!strcmp(name,"vkCmdPushDescriptorSet2KHR"))name="vkCmdPushDescriptorSet2";
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplateKHR"))name="vkCmdPushDescriptorSetWithTemplate";
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplate2KHR"))name="vkCmdPushDescriptorSetWithTemplate2";
    if(!strcmp(name,"vkCreateDescriptorUpdateTemplateKHR"))name="vkCreateDescriptorUpdateTemplate";
    if(!strcmp(name,"vkDestroyDescriptorUpdateTemplateKHR"))name="vkDestroyDescriptorUpdateTemplate";
    if(!strcmp(name,"vkUpdateDescriptorSetWithTemplateKHR"))name="vkUpdateDescriptorSetWithTemplate";
    if(!strcmp(name,"vkQueueSubmit2KHR"))name="vkQueueSubmit2";
    null_device *device=lookup(handle);
    if(!device)return NULL;
    if(!strcmp(name,"vkDestroyDescriptorPool") && device->nulldesc->precise)return (PFN_vkVoidFunction)nd_DestroyDescriptorPool;
    if(!strcmp(name,"vkResetDescriptorPool") && device->nulldesc->precise)return (PFN_vkVoidFunction)nd_ResetDescriptorPool;
    if(!strcmp(name,"vkFreeDescriptorSets") && device->nulldesc->precise)return (PFN_vkVoidFunction)nd_FreeDescriptorSets;
    if(!strcmp(name,"vkAllocateDescriptorSets") && device->dt.vkAllocateDescriptorSets==nd_AllocateDescriptorSets)return (PFN_vkVoidFunction)nd_AllocateDescriptorSets;
    if(!strcmp(name,"vkCmdBindVertexBuffers") && device->dt.vkCmdBindVertexBuffers==nd_CmdBindVertexBuffers)return (PFN_vkVoidFunction)nd_CmdBindVertexBuffers;
    if(!strcmp(name,"vkCmdBindVertexBuffers2") && device->dt.vkCmdBindVertexBuffers2==nd_CmdBindVertexBuffers2)return (PFN_vkVoidFunction)nd_CmdBindVertexBuffers2;
    if(!strcmp(name,"vkCmdPushDescriptorSet") && device->dt.vkCmdPushDescriptorSet==nd_CmdPushDescriptorSet)return (PFN_vkVoidFunction)nd_CmdPushDescriptorSet;
    if(!strcmp(name,"vkCmdPushDescriptorSet2") && device->dt.vkCmdPushDescriptorSet2==nd_CmdPushDescriptorSet2)return (PFN_vkVoidFunction)nd_CmdPushDescriptorSet2;
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplate2") && device->dt.vkCmdPushDescriptorSetWithTemplate2==nd_CmdPushDescriptorSetWithTemplate2)return (PFN_vkVoidFunction)nd_CmdPushDescriptorSetWithTemplate2;
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplate") && device->dt.vkCmdPushDescriptorSetWithTemplate==nd_CmdPushDescriptorSetWithTemplate)return (PFN_vkVoidFunction)nd_CmdPushDescriptorSetWithTemplate;
    if(!strcmp(name,"vkCreateComputePipelines") && device->dt.vkCreateComputePipelines==nd_CreateComputePipelines)return (PFN_vkVoidFunction)nd_CreateComputePipelines;
    if(!strcmp(name,"vkCreateDescriptorUpdateTemplate") && device->dt.vkCreateDescriptorUpdateTemplate==nd_CreateDescriptorUpdateTemplate)return (PFN_vkVoidFunction)nd_CreateDescriptorUpdateTemplate;
    if(!strcmp(name,"vkCreateGraphicsPipelines") && device->dt.vkCreateGraphicsPipelines==nd_CreateGraphicsPipelines)return (PFN_vkVoidFunction)nd_CreateGraphicsPipelines;
    if(!strcmp(name,"vkCreatePipelineLayout") && device->dt.vkCreatePipelineLayout==nd_CreatePipelineLayout)return (PFN_vkVoidFunction)nd_CreatePipelineLayout;
    if(!strcmp(name,"vkCreateShaderModule") && device->dt.vkCreateShaderModule==nd_CreateShaderModule)return (PFN_vkVoidFunction)nd_CreateShaderModule;
    if(!strcmp(name,"vkDestroyDescriptorSetLayout") && device->dt.vkDestroyDescriptorSetLayout==nd_DestroyDescriptorSetLayout)return (PFN_vkVoidFunction)nd_DestroyDescriptorSetLayout;
    if(!strcmp(name,"vkDestroyDescriptorUpdateTemplate") && device->dt.vkDestroyDescriptorUpdateTemplate==nd_DestroyDescriptorUpdateTemplate)return (PFN_vkVoidFunction)nd_DestroyDescriptorUpdateTemplate;
    if(!strcmp(name,"vkDestroyPipelineLayout") && device->dt.vkDestroyPipelineLayout==nd_DestroyPipelineLayout)return (PFN_vkVoidFunction)nd_DestroyPipelineLayout;
    if(!strcmp(name,"vkDestroyShaderModule") && device->dt.vkDestroyShaderModule==nd_DestroyShaderModule)return (PFN_vkVoidFunction)nd_DestroyShaderModule;
    if(!strcmp(name,"vkGetDeviceQueue") && device->dt.vkGetDeviceQueue==nd_GetDeviceQueue)return (PFN_vkVoidFunction)nd_GetDeviceQueue;
    if(!strcmp(name,"vkGetDeviceQueue2") && device->dt.vkGetDeviceQueue2==nd_GetDeviceQueue2)return (PFN_vkVoidFunction)nd_GetDeviceQueue2;
    if(!strcmp(name,"vkQueueSubmit") && device->dt.vkQueueSubmit==nd_QueueSubmit)return (PFN_vkVoidFunction)nd_QueueSubmit;
    if(!strcmp(name,"vkQueueSubmit2") && device->dt.vkQueueSubmit2==nd_QueueSubmit2)return (PFN_vkVoidFunction)nd_QueueSubmit2;
    if(!strcmp(name,"vkUpdateDescriptorSetWithTemplate") && device->dt.vkUpdateDescriptorSetWithTemplate==nd_UpdateDescriptorSetWithTemplate)return (PFN_vkVoidFunction)nd_UpdateDescriptorSetWithTemplate;
    if(!strcmp(name,"vkUpdateDescriptorSets") && device->dt.vkUpdateDescriptorSets==nd_UpdateDescriptorSets)return (PFN_vkVoidFunction)nd_UpdateDescriptorSets;
    return NULL;
}

/* Instance and device resolvers share these per-handle routing addresses. */
static VkResult VKAPI_CALL route_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                 VkShaderModule *out)
{
    return ((PFN_vkCreateShaderModule)hybris_layer_handle_inner_proc(device,"vkCreateShaderModule"))(device,ci,a,out);
}
static void VKAPI_CALL route_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *a)
{
    ((PFN_vkDestroyShaderModule)hybris_layer_handle_inner_proc(device,"vkDestroyShaderModule"))(device,m,a);
}
static VkResult VKAPI_CALL route_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a,
                                                   VkPipelineLayout *out)
{
    return ((PFN_vkCreatePipelineLayout)hybris_layer_handle_inner_proc(device,"vkCreatePipelineLayout"))(device,ci,a,out);
}
static void VKAPI_CALL route_DestroyPipelineLayout(VkDevice device, VkPipelineLayout pl, const VkAllocationCallbacks *a)
{
    ((PFN_vkDestroyPipelineLayout)hybris_layer_handle_inner_proc(device,"vkDestroyPipelineLayout"))(device,pl,a);
}
static void VKAPI_CALL route_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout dsl, const VkAllocationCallbacks *a)
{
    ((PFN_vkDestroyDescriptorSetLayout)hybris_layer_handle_inner_proc(device,"vkDestroyDescriptorSetLayout"))(device,dsl,a);
}
static VkResult VKAPI_CALL route_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
    return ((PFN_vkCreateGraphicsPipelines)hybris_layer_handle_inner_proc(device,"vkCreateGraphicsPipelines"))(device,cache,count,ci,a,out);
}
static VkResult VKAPI_CALL route_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                     const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
    return ((PFN_vkCreateComputePipelines)hybris_layer_handle_inner_proc(device,"vkCreateComputePipelines"))(device,cache,count,ci,a,out);
}
static VkResult VKAPI_CALL route_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    return ((PFN_vkAllocateDescriptorSets)hybris_layer_handle_inner_proc(device,"vkAllocateDescriptorSets"))(device,ai,out);
}
static VkResult VKAPI_CALL route_FreeDescriptorSets(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet *sets)
{
    return ((PFN_vkFreeDescriptorSets)hybris_layer_handle_inner_proc(device,"vkFreeDescriptorSets"))(device,pool,count,sets);
}
static VkResult VKAPI_CALL route_ResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags)
{
    return ((PFN_vkResetDescriptorPool)hybris_layer_handle_inner_proc(device,"vkResetDescriptorPool"))(device,pool,flags);
}
static void VKAPI_CALL route_DestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks *allocator)
{
    ((PFN_vkDestroyDescriptorPool)hybris_layer_handle_inner_proc(device,"vkDestroyDescriptorPool"))(device,pool,allocator);
}
static void VKAPI_CALL route_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                               const VkCopyDescriptorSet *cp)
{
    ((PFN_vkUpdateDescriptorSets)hybris_layer_handle_inner_proc(device,"vkUpdateDescriptorSets"))(device,nw,w,nc,cp);
}
static VkResult VKAPI_CALL route_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    return ((PFN_vkCreateDescriptorUpdateTemplate)hybris_layer_handle_inner_proc(device,"vkCreateDescriptorUpdateTemplate"))(device,ci,a,out);
}
static void VKAPI_CALL route_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks *a)
{
    ((PFN_vkDestroyDescriptorUpdateTemplate)hybris_layer_handle_inner_proc(device,"vkDestroyDescriptorUpdateTemplate"))(device,tmpl,a);
}
static void VKAPI_CALL route_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    ((PFN_vkUpdateDescriptorSetWithTemplate)hybris_layer_handle_inner_proc(device,"vkUpdateDescriptorSetWithTemplate"))(device,set,tmpl,data);
}
static void VKAPI_CALL route_CmdPushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                               uint32_t nw, const VkWriteDescriptorSet *w)
{
    ((PFN_vkCmdPushDescriptorSet)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSet"))(cb,bp,layout,set,nw,w);
}
static void VKAPI_CALL route_CmdPushDescriptorSet2(VkCommandBuffer cb, const VkPushDescriptorSetInfo *info)
{
    ((PFN_vkCmdPushDescriptorSet2)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSet2"))(cb,info);
}
static void VKAPI_CALL route_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                           uint32_t set, const void *data)
{
    ((PFN_vkCmdPushDescriptorSetWithTemplate)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSetWithTemplate"))(cb,tmpl,layout,set,data);
}
static void VKAPI_CALL route_CmdPushDescriptorSetWithTemplate2(VkCommandBuffer cb,const VkPushDescriptorSetWithTemplateInfo *info)
{
    ((PFN_vkCmdPushDescriptorSetWithTemplate2)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSetWithTemplate2"))(cb,info);
}
static void VKAPI_CALL route_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                               const VkDeviceSize *offs)
{
    ((PFN_vkCmdBindVertexBuffers)hybris_layer_handle_inner_proc(cb,"vkCmdBindVertexBuffers"))(cb,first,count_,bufs,offs);
}
static void VKAPI_CALL route_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                                const VkDeviceSize *offs, const VkDeviceSize *sizes, const VkDeviceSize *strides)
{
    ((PFN_vkCmdBindVertexBuffers2)hybris_layer_handle_inner_proc(cb,"vkCmdBindVertexBuffers2"))(cb,first,count_,bufs,offs,sizes,strides);
}
static void VKAPI_CALL route_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *q)
{
    ((PFN_vkGetDeviceQueue)hybris_layer_handle_inner_proc(device,"vkGetDeviceQueue"))(device,family,index,q);
}
static void VKAPI_CALL route_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
    ((PFN_vkGetDeviceQueue2)hybris_layer_handle_inner_proc(device,"vkGetDeviceQueue2"))(device,info,q);
}
static VkResult VKAPI_CALL route_QueueSubmit(VkQueue q, uint32_t count_, const VkSubmitInfo *s, VkFence f)
{
    return ((PFN_vkQueueSubmit)hybris_layer_handle_inner_proc(q,"vkQueueSubmit"))(q,count_,s,f);
}
static VkResult VKAPI_CALL route_QueueSubmit2(VkQueue q, uint32_t count_, const VkSubmitInfo2 *s, VkFence f)
{
    return ((PFN_vkQueueSubmit2)hybris_layer_handle_inner_proc(q,"vkQueueSubmit2"))(q,count_,s,f);
}
static VkResult VKAPI_CALL route_CreateDescriptorUpdateTemplateKHR(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    return ((PFN_vkCreateDescriptorUpdateTemplate)hybris_layer_handle_inner_proc(device,"vkCreateDescriptorUpdateTemplateKHR"))(device,ci,a,out);
}
static void VKAPI_CALL route_DestroyDescriptorUpdateTemplateKHR(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks *a)
{
    ((PFN_vkDestroyDescriptorUpdateTemplate)hybris_layer_handle_inner_proc(device,"vkDestroyDescriptorUpdateTemplateKHR"))(device,tmpl,a);
}
static void VKAPI_CALL route_UpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    ((PFN_vkUpdateDescriptorSetWithTemplate)hybris_layer_handle_inner_proc(device,"vkUpdateDescriptorSetWithTemplateKHR"))(device,set,tmpl,data);
}
static void VKAPI_CALL route_CmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                               uint32_t nw, const VkWriteDescriptorSet *w)
{
    ((PFN_vkCmdPushDescriptorSet)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSetKHR"))(cb,bp,layout,set,nw,w);
}
static void VKAPI_CALL route_CmdPushDescriptorSet2KHR(VkCommandBuffer cb, const VkPushDescriptorSetInfo *info)
{
    ((PFN_vkCmdPushDescriptorSet2)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSet2KHR"))(cb,info);
}
static void VKAPI_CALL route_CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                           uint32_t set, const void *data)
{
    ((PFN_vkCmdPushDescriptorSetWithTemplate)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSetWithTemplateKHR"))(cb,tmpl,layout,set,data);
}
static void VKAPI_CALL route_CmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer cb,const VkPushDescriptorSetWithTemplateInfo *info)
{
    ((PFN_vkCmdPushDescriptorSetWithTemplate2)hybris_layer_handle_inner_proc(cb,"vkCmdPushDescriptorSetWithTemplate2KHR"))(cb,info);
}
static void VKAPI_CALL route_CmdBindVertexBuffers2EXT(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                                const VkDeviceSize *offs, const VkDeviceSize *sizes, const VkDeviceSize *strides)
{
    ((PFN_vkCmdBindVertexBuffers2)hybris_layer_handle_inner_proc(cb,"vkCmdBindVertexBuffers2EXT"))(cb,first,count_,bufs,offs,sizes,strides);
}
static VkResult VKAPI_CALL route_QueueSubmit2KHR(VkQueue q, uint32_t count_, const VkSubmitInfo2 *s, VkFence f)
{
    return ((PFN_vkQueueSubmit2)hybris_layer_handle_inner_proc(q,"vkQueueSubmit2KHR"))(q,count_,s,f);
}
PFN_vkVoidFunction hybris_null_dispatch(const char *name)
{
    if(!strcmp(name,"vkCreateShaderModule"))return (PFN_vkVoidFunction)route_CreateShaderModule;
    if(!strcmp(name,"vkDestroyShaderModule"))return (PFN_vkVoidFunction)route_DestroyShaderModule;
    if(!strcmp(name,"vkCreatePipelineLayout"))return (PFN_vkVoidFunction)route_CreatePipelineLayout;
    if(!strcmp(name,"vkDestroyPipelineLayout"))return (PFN_vkVoidFunction)route_DestroyPipelineLayout;
    if(!strcmp(name,"vkDestroyDescriptorSetLayout"))return (PFN_vkVoidFunction)route_DestroyDescriptorSetLayout;
    if(!strcmp(name,"vkCreateGraphicsPipelines"))return (PFN_vkVoidFunction)route_CreateGraphicsPipelines;
    if(!strcmp(name,"vkCreateComputePipelines"))return (PFN_vkVoidFunction)route_CreateComputePipelines;
    if(!strcmp(name,"vkAllocateDescriptorSets"))return (PFN_vkVoidFunction)route_AllocateDescriptorSets;
    if(!strcmp(name,"vkFreeDescriptorSets"))return (PFN_vkVoidFunction)route_FreeDescriptorSets;
    if(!strcmp(name,"vkResetDescriptorPool"))return (PFN_vkVoidFunction)route_ResetDescriptorPool;
    if(!strcmp(name,"vkDestroyDescriptorPool"))return (PFN_vkVoidFunction)route_DestroyDescriptorPool;
    if(!strcmp(name,"vkUpdateDescriptorSets"))return (PFN_vkVoidFunction)route_UpdateDescriptorSets;
    if(!strcmp(name,"vkCreateDescriptorUpdateTemplateKHR"))return (PFN_vkVoidFunction)route_CreateDescriptorUpdateTemplateKHR;
    if(!strcmp(name,"vkCreateDescriptorUpdateTemplate"))return (PFN_vkVoidFunction)route_CreateDescriptorUpdateTemplate;
    if(!strcmp(name,"vkDestroyDescriptorUpdateTemplateKHR"))return (PFN_vkVoidFunction)route_DestroyDescriptorUpdateTemplateKHR;
    if(!strcmp(name,"vkDestroyDescriptorUpdateTemplate"))return (PFN_vkVoidFunction)route_DestroyDescriptorUpdateTemplate;
    if(!strcmp(name,"vkUpdateDescriptorSetWithTemplateKHR"))return (PFN_vkVoidFunction)route_UpdateDescriptorSetWithTemplateKHR;
    if(!strcmp(name,"vkUpdateDescriptorSetWithTemplate"))return (PFN_vkVoidFunction)route_UpdateDescriptorSetWithTemplate;
    if(!strcmp(name,"vkCmdPushDescriptorSetKHR"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSetKHR;
    if(!strcmp(name,"vkCmdPushDescriptorSet"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSet;
    if(!strcmp(name,"vkCmdPushDescriptorSet2KHR"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSet2KHR;
    if(!strcmp(name,"vkCmdPushDescriptorSet2"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSet2;
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplateKHR"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSetWithTemplateKHR;
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplate"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSetWithTemplate;
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplate2KHR"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSetWithTemplate2KHR;
    if(!strcmp(name,"vkCmdPushDescriptorSetWithTemplate2"))return (PFN_vkVoidFunction)route_CmdPushDescriptorSetWithTemplate2;
    if(!strcmp(name,"vkCmdBindVertexBuffers"))return (PFN_vkVoidFunction)route_CmdBindVertexBuffers;
    if(!strcmp(name,"vkCmdBindVertexBuffers2EXT"))return (PFN_vkVoidFunction)route_CmdBindVertexBuffers2EXT;
    if(!strcmp(name,"vkCmdBindVertexBuffers2"))return (PFN_vkVoidFunction)route_CmdBindVertexBuffers2;
    if(!strcmp(name,"vkGetDeviceQueue"))return (PFN_vkVoidFunction)route_GetDeviceQueue;
    if(!strcmp(name,"vkGetDeviceQueue2"))return (PFN_vkVoidFunction)route_GetDeviceQueue2;
    if(!strcmp(name,"vkQueueSubmit"))return (PFN_vkVoidFunction)route_QueueSubmit;
    if(!strcmp(name,"vkQueueSubmit2KHR"))return (PFN_vkVoidFunction)route_QueueSubmit2KHR;
    if(!strcmp(name,"vkQueueSubmit2"))return (PFN_vkVoidFunction)route_QueueSubmit2;
    return NULL;
}
