/* SPDX-License-Identifier: Apache-2.0 */
#define VK_NO_PROTOTYPES
#include "vertex_stores.h"
#include "../layer/layer.h"
#include <pthread.h>
#include <string.h>
#include <sys/auxv.h>

static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static int enabled = 1;
static void configure(void)
{
    const char *control = getauxval(AT_SECURE) ? NULL : getenv("HYBRIS_VULKAN_COMPAT_VERTEX_STORES");
    enabled = !control || strcmp(control, "0");
}
int hybris_vertex_stores_active(VkPhysicalDevice physical)
{
    pthread_once(&config_once, configure);
    if (!enabled) return 0;
    struct hybris_layer_physical context;
    if (!hybris_layer_lookup_physical(physical, &context)) return 0;
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)
        context.resolver(context.instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures features = (PFN_vkGetPhysicalDeviceFeatures)
        context.resolver(context.instance, "vkGetPhysicalDeviceFeatures");
    if (!properties || !features) return 0;
    VkPhysicalDeviceProperties p;
    VkPhysicalDeviceFeatures f;
    properties(physical, &p); features(physical, &f);
    /* Inspected G1-Ultra driver only. Older Mali generations can issue
     * speculative vertex invocations for indices absent from the draw. */
    return !f.vertexPipelineStoresAndAtomics && p.vendorID == 0x13b5 &&
        p.deviceID == 0xe8800010 && p.driverVersion == 0x0d801000;
}

struct render_pass {
    VkDevice device;
    VkRenderPass handle;
    uint32_t count;
    VkAllocationCallbacks allocator;
    int custom;
    struct render_pass *next;
    struct hybris_vertex_rendering subpasses[];
};
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static struct render_pass *passes;

static struct render_pass *allocate_pass(VkDevice device, uint32_t count,
    const VkAllocationCallbacks *allocator)
{
    if (sizeof(struct hybris_vertex_rendering) > (SIZE_MAX - sizeof(struct render_pass)) / (count ? count : 1)) return NULL;
    struct render_pass *p = hybris_scaled_alloc(allocator,
        sizeof(*p) + count * sizeof(*p->subpasses), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!p) return NULL;
    *p = (struct render_pass){.device = device, .count = count, .custom = allocator != NULL};
    if (allocator) p->allocator = *allocator;
    return p;
}
static void publish_pass(struct render_pass *p, VkRenderPass handle)
{
    p->handle = handle;
    pthread_mutex_lock(&guard);
    p->next = passes; passes = p;
    pthread_mutex_unlock(&guard);
}
static VkResult VKAPI_CALL create_pass(VkDevice handle, const VkRenderPassCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkRenderPass *out)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateRenderPass create = (PFN_vkCreateRenderPass)device.resolver(handle, "vkCreateRenderPass");
    if (!hybris_vertex_stores_active(device.physical)) return create(handle, info, allocator, out);
    *out = VK_NULL_HANDLE;
    struct render_pass *p = allocate_pass(handle, info->subpassCount, allocator);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const VkSubpassDescription *s = &info->pSubpasses[i];
        p->subpasses[i] = (struct hybris_vertex_rendering){s->colorAttachmentCount, VK_SAMPLE_COUNT_1_BIT};
        for (uint32_t j = 0; j < s->colorAttachmentCount; ++j)
            if (s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED)
                p->subpasses[i].samples = info->pAttachments[s->pColorAttachments[j].attachment].samples;
        if (s->pDepthStencilAttachment && s->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
            p->subpasses[i].samples = info->pAttachments[s->pDepthStencilAttachment->attachment].samples;
    }
    VkResult result = create(handle, info, allocator, out);
    if (result == VK_SUCCESS) publish_pass(p, *out);
    else hybris_scaled_free(allocator, p);
    return result;
}
static VkResult create_pass2(VkDevice handle, const VkRenderPassCreateInfo2 *info,
    const VkAllocationCallbacks *allocator, VkRenderPass *out, const char *name)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateRenderPass2 create = (PFN_vkCreateRenderPass2)device.resolver(handle, name);
    if (!hybris_vertex_stores_active(device.physical)) return create(handle, info, allocator, out);
    *out = VK_NULL_HANDLE;
    struct render_pass *p = allocate_pass(handle, info->subpassCount, allocator);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const VkSubpassDescription2 *s = &info->pSubpasses[i];
        p->subpasses[i] = (struct hybris_vertex_rendering){s->colorAttachmentCount, VK_SAMPLE_COUNT_1_BIT};
        for (uint32_t j = 0; j < s->colorAttachmentCount; ++j)
            if (s->pColorAttachments[j].attachment != VK_ATTACHMENT_UNUSED)
                p->subpasses[i].samples = info->pAttachments[s->pColorAttachments[j].attachment].samples;
        if (s->pDepthStencilAttachment && s->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
            p->subpasses[i].samples = info->pAttachments[s->pDepthStencilAttachment->attachment].samples;
    }
    VkResult result = create(handle, info, allocator, out);
    if (result == VK_SUCCESS) publish_pass(p, *out);
    else hybris_scaled_free(allocator, p);
    return result;
}
static VkResult VKAPI_CALL create_pass2_core(VkDevice d, const VkRenderPassCreateInfo2 *i,
    const VkAllocationCallbacks *a, VkRenderPass *o)
{ return create_pass2(d, i, a, o, "vkCreateRenderPass2"); }
static VkResult VKAPI_CALL create_pass2_khr(VkDevice d, const VkRenderPassCreateInfo2 *i,
    const VkAllocationCallbacks *a, VkRenderPass *o)
{ return create_pass2(d, i, a, o, "vkCreateRenderPass2KHR"); }
static void VKAPI_CALL destroy_pass(VkDevice handle, VkRenderPass pass,
    const VkAllocationCallbacks *allocator)
{
    struct hybris_layer_device device;
    if (!hybris_layer_lookup_device(handle, &device)) return;
    pthread_mutex_lock(&guard);
    struct render_pass **link = &passes;
    while (*link && ((*link)->device != handle || (*link)->handle != pass)) link = &(*link)->next;
    struct render_pass *p = *link;
    if (p) *link = p->next;
    pthread_mutex_unlock(&guard);
    ((PFN_vkDestroyRenderPass)device.resolver(handle, "vkDestroyRenderPass"))(handle, pass, allocator);
    if (p) hybris_scaled_free(p->custom ? &p->allocator : NULL, p);
}
void hybris_vertex_stores_release_device(VkDevice handle)
{
    struct render_pass *retired = NULL;
    pthread_mutex_lock(&guard);
    struct render_pass **link = &passes;
    while (*link) {
        struct render_pass *p = *link;
        if (p->device != handle) { link = &p->next; continue; }
        *link = p->next;
        p->next = retired; retired = p;
    }
    pthread_mutex_unlock(&guard);
    while (retired) {
        struct render_pass *p = retired; retired = p->next;
        hybris_scaled_free(p->custom ? &p->allocator : NULL, p);
    }
}
VkResult hybris_vertex_rendering_info(VkDevice handle,
    const VkGraphicsPipelineCreateInfo *info, struct hybris_vertex_rendering *out)
{
    if (info->renderPass) {
        VkResult result = VK_ERROR_UNKNOWN;
        pthread_mutex_lock(&guard);
        for (struct render_pass *p = passes; p; p = p->next)
            if (p->device == handle && p->handle == info->renderPass && info->subpass < p->count) {
                *out = p->subpasses[info->subpass]; result = VK_SUCCESS; break;
            }
        pthread_mutex_unlock(&guard);
        return result;
    }
    /* Dynamic rendering has no attachment sample counts at pipeline creation.
     * Require an explicit sample count instead of guessing attachment state. */
    if (!info->pMultisampleState) return VK_ERROR_UNKNOWN;
    for (const VkBaseInStructure *node = info->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            *out = (struct hybris_vertex_rendering){
                ((const VkPipelineRenderingCreateInfo *)node)->colorAttachmentCount,
                info->pMultisampleState->rasterizationSamples};
            return VK_SUCCESS;
        }
    return VK_ERROR_UNKNOWN;
}
PFN_vkVoidFunction hybris_vertex_stores_proc(const char *name)
{
    if (!strcmp(name, "vkCreateRenderPass")) return (PFN_vkVoidFunction)create_pass;
    if (!strcmp(name, "vkCreateRenderPass2")) return (PFN_vkVoidFunction)create_pass2_core;
    if (!strcmp(name, "vkCreateRenderPass2KHR")) return (PFN_vkVoidFunction)create_pass2_khr;
    if (!strcmp(name, "vkDestroyRenderPass")) return (PFN_vkVoidFunction)destroy_pass;
    return NULL;
}
