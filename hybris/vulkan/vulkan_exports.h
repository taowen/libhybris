#ifndef HYBRIS_VULKAN_EXPORTS_H
#define HYBRIS_VULKAN_EXPORTS_H

/* Called once by the frontend constructor, after Android libvulkan is loaded. */
__attribute__((visibility("hidden")))
void hybris_vulkan_resolve_exports(void *android_handle);

#endif
