/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include "scaled_formats.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

const struct hybris_scaled_format_pair hybris_scaled_formats[HYBRIS_SCALED_FORMAT_COUNT] = {
#define PAIR(c) { VK_FORMAT_##c##_USCALED, VK_FORMAT_##c##_UINT, 0, 0 }, \
                { VK_FORMAT_##c##_SSCALED, VK_FORMAT_##c##_SINT, 1, 0 }
    PAIR(R8), PAIR(R8G8), PAIR(R8G8B8A8), PAIR(R16), PAIR(R16G16), PAIR(R16G16B16A16),
    { VK_FORMAT_A2R10G10B10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 0, 1 }
#undef PAIR
};
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static int enabled, force, packed_enabled, packed_force, trace;
static _Atomic unsigned traced_devices;
static void configure(void)
{
    if (getauxval(AT_SECURE)) return;
    const char *value = getenv("HYBRIS_VULKAN_COMPAT_SCALED_VERTEX");
    force = value && !strcmp(value, "force");
    enabled = force || (value && !strcmp(value, "1"));
    value = getenv("HYBRIS_VULKAN_COMPAT_PACKED_VERTEX");
    packed_force = value && !strcmp(value, "force");
    packed_enabled = packed_force || (value && !strcmp(value, "1"));
    value = getenv("HYBRIS_VULKAN_COMPAT_FORMAT_TRACE");
    trace = value && !strcmp(value, "1");
}
int hybris_scaled_enabled(void) { pthread_once(&config_once, configure); return enabled || packed_enabled; }

struct decision {
    VkFormatProperties raw, fetch, effective;
    int fallback;
    const char *reason;
};
static struct decision decide(PFN_vkGetPhysicalDeviceFormatProperties query,
    VkPhysicalDevice physical, unsigned i)
{
    struct decision d = {0};
    query(physical, hybris_scaled_formats[i].scaled, &d.raw);
    query(physical, hybris_scaled_formats[i].integer, &d.fetch);
    d.effective = d.raw;
    int swizzle = hybris_scaled_formats[i].rb_swizzle;
    int active = swizzle ? packed_enabled : enabled;
    int forced = swizzle ? packed_force : force;
    int native = !!(d.raw.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    int integer = !!(d.fetch.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    d.fallback = active && (forced || !native) && integer;
    d.reason = !active ? "disabled" : !forced && native ? "native-scaled" : !integer ? "integer-fetch-unavailable" :
               forced ? "forced-integer-fetch" : "missing-scaled-fetch";
    if (swizzle && active)
        d.reason = !forced && native ? "native-packed" : !integer ? "swizzled-fetch-unavailable" :
                   forced ? "forced-swizzled-fetch" : "missing-packed-fetch";
    if (d.fallback) d.effective.bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    return d;
}
void hybris_scaled_format(PFN_vkGetPhysicalDeviceFormatProperties query,
    VkPhysicalDevice physical, VkFormat format, VkFormatProperties *properties)
{
    if (!hybris_scaled_enabled() || !query) return;
    for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i)
        if (hybris_scaled_formats[i].scaled == format && decide(query, physical, i).fallback)
            properties->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}
unsigned hybris_scaled_physical_mask(VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceFormatProperties query)
{
    unsigned mask = 0;
    if (!hybris_scaled_enabled() || !query) return 0;
    for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i) {
        if (hybris_scaled_formats[i].rb_swizzle ? !packed_enabled : !enabled) continue;
        if (decide(query, physical, i).fallback) mask |= 1u << i;
    }
    return mask;
}
unsigned hybris_scaled_mask(VkDevice device, VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceFormatProperties query)
{
    if (!hybris_scaled_enabled()) return 0;
    /* At most 16 complete device snapshots and one truncation notice. Query
     * and draw paths never emit records; no driver call runs under a lock. */
    unsigned slot = 17;
    if (trace) {
        slot = atomic_load(&traced_devices);
        while (slot < 17 && !atomic_compare_exchange_weak(&traced_devices, &slot, slot + 1)) {}
        if (slot == 16) fprintf(stderr, "HYBRIS_SCALED_FORMAT truncated\n");
    }
    unsigned mask = 0;
    if (query) for (unsigned i = 0; i < HYBRIS_SCALED_FORMAT_COUNT; ++i) {
        if (hybris_scaled_formats[i].rb_swizzle ? !packed_enabled : !enabled) continue;
        struct decision d = decide(query, physical, i);
        if (d.fallback) mask |= 1u << i;
        if (slot < 16) fprintf(stderr,
            "HYBRIS_SCALED_FORMAT device=%p physical=%p index=%u scaled=%d integer=%d "
            "raw=0x%x,0x%x,0x%x fetch=0x%x,0x%x,0x%x effective=0x%x,0x%x,0x%x fallback=%d reason=%s\n",
            (void *)device, (void *)physical, i, hybris_scaled_formats[i].scaled, hybris_scaled_formats[i].integer,
            d.raw.linearTilingFeatures, d.raw.optimalTilingFeatures, d.raw.bufferFeatures,
            d.fetch.linearTilingFeatures, d.fetch.optimalTilingFeatures, d.fetch.bufferFeatures,
            d.effective.linearTilingFeatures, d.effective.optimalTilingFeatures, d.effective.bufferFeatures,
            d.fallback, d.reason);
    }
    fprintf(stderr, "HYBRIS_SCALED_VERTEX experimental=1 force=%d fallback_mask=0x%x\n", force, mask);
    return mask;
}
