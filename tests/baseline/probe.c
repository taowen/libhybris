#include "probe.h"

__attribute__((constructor))
static void probe_watchdog(void) {
  alarm(25);
}

int main(int argc, char **argv) {
  setbuf(stdout, NULL);
  const char *mode = argc > 1 ? argv[1] : "vk";
  printf("PROBE pid=%d mode=%s linked=%d\n", getpid(), mode,
#ifdef HYBRIS_PROBE_LINKED
         1
#else
         0
#endif
  );
  int rc;
  if (!strcmp(mode, "version"))
    rc = icd_version_probe();
  else if (!strcmp(mode, "tls-mrs"))
    rc = tls_mrs_probe();
  else if (!strcmp(mode, "groups"))
    rc = groups_probe(0);
  else if (!strcmp(mode, "groups-dlsym"))
    rc = groups_probe(1);
  else if (!strcmp(mode, "egl-vulkan")) {
    puts("MIXED_API phase=egl-first");
    rc = eglprobe(3);
    if (!rc) { puts("MIXED_API phase=vulkan-second"); rc = ubo_probe(); }
  } else if (!strcmp(mode, "vulkan-egl")) {
    puts("MIXED_API phase=vulkan-first");
    rc = ubo_probe();
    if (!rc) { puts("MIXED_API phase=egl-second"); rc = eglprobe(3); }
  }
  else if (!strcmp(mode, "egl-life"))
    rc = egl_lifecycle_probe();
  else if (!strcmp(mode, "dispatch"))
    rc = dispatch_probe();
  else if (!strcmp(mode, "life"))
    rc = life_probe(0);
  else if (!strcmp(mode, "unload"))
    rc = life_probe(1);
  else if (!strcmp(mode, "init"))
    rc = init_probe();
  else if (!strcmp(mode, "stdio"))
    rc = stdio_probe();
  else if (!strcmp(mode, "vk-alloc"))
    rc = vulkan_allocator_probe(0);
  else if (!strcmp(mode, "icd-alloc-direct"))
    rc = vulkan_allocator_probe(1);
  else if (!strcmp(mode, "vk-init"))
    rc = vulkan_init_probe();
  else if (!strcmp(mode, "mutex-init"))
    rc = lock_init_probe(0);
  else if (!strcmp(mode, "rwlock-init"))
    rc = lock_init_probe(1);
  else if (!strcmp(mode, "rwlock-monotonic"))
    rc = rwlock_monotonic_probe();
  else if (!strcmp(mode, "mutex-monotonic"))
    rc = mutex_monotonic_probe();
  else if (!strcmp(mode, "sync-destroy"))
    rc = sync_destroy_probe(0);
  else if (!strcmp(mode, "rwlock-kind"))
    rc = sync_destroy_probe(1);
  else if (!strcmp(mode, "cond-init"))
    rc = cond_init_probe();
  else if (!strcmp(mode, "cond-clock"))
    rc = cond_clock_probe();
  else if (!strcmp(mode, "shared-unavailable"))
    rc = shared_unavailable_probe();
  else if (!strcmp(mode, "tls-dtor"))
    rc = tls_destructor_probe();
  else if (!strcmp(mode, "tls-bounds"))
    rc = tls_bounds_probe();
  else if (!strcmp(mode, "tls"))
    rc = tls_probe();
  else if (!strcmp(mode, "native-buffer"))
    rc = native_buffer_probe();
  else if (!strncmp(mode, "vertex-policy", 13))
    rc = vertex_policy_probe(strstr(mode, "direct") != NULL, strstr(mode, "restricted") != NULL);
  else if (!strcmp(mode, "caps2"))
    rc = caps2_probe();
  else if (!strcmp(mode, "caps"))
    rc = caps_probe(0);
  else if (!strcmp(mode, "blender-vk"))
    rc = blender_vk_probe(0);
  else if (!strcmp(mode, "blender-vk-5.2"))
    rc = blender_vk_probe(1);
  else if (!strcmp(mode, "wsi-disabled"))
    rc = caps_probe(1);
  else if (!strcmp(mode, "vertex-store-raw"))
    rc = vertex_store_probe(1, 0);
  else if (!strcmp(mode, "vertex-store-raw-enabled"))
    rc = vertex_store_probe(2, 0);
  else if (!strcmp(mode, "vertex-store"))
    rc = vertex_store_probe(0, 0);
  else if (!strcmp(mode, "vertex-store-features2-validation"))
    rc = vertex_store_probe(0, 2);
  else if (!strcmp(mode, "vertex-store-validation"))
    rc = vertex_store_probe(0, 1);
  else if (!strncmp(mode, "point-size", 10))
    rc = point_size_probe(strstr(mode, "validation") != NULL,
      strstr(mode, "gdpa") ? 1 : strstr(mode, "elf") ? 2 : strstr(mode, "linked") ? 3 : 0);
  else if (!strncmp(mode, "scaled-vertex", 13))
    rc = scaled_vertex_probe(strstr(mode, "validation") != NULL,
      strstr(mode, "gdpa") ? 1 : strstr(mode, "elf") ? 2 : strstr(mode, "linked") ? 3 : 0, mode);
  else if (!strcmp(mode, "ubo-template"))
    rc = ubo_template_probe(0);
  else if (!strcmp(mode, "ubo-template-validation"))
    rc = ubo_template_probe(1);
  else if (!strcmp(mode, "ubo-pool-reset") || !strcmp(mode, "ubo-pool-reset-validation"))
    rc = ubo_pool_reset_probe(strstr(mode, "validation") != NULL, 0);
  else if (!strcmp(mode, "ubo-pool-empty-reset") || !strcmp(mode, "ubo-pool-empty-reset-validation"))
    rc = ubo_pool_reset_probe(strstr(mode, "validation") != NULL, 1);
  else if (!strcmp(mode, "ubo-staged"))
    rc = ubo_staged_probe(0);
  else if (!strcmp(mode, "ubo-staged-validation"))
    rc = ubo_staged_probe(1);
  else if (!strcmp(mode, "ubo-large"))
    rc = ubo_large_probe(0);
  else if (!strcmp(mode, "ubo-large-validation"))
    rc = ubo_large_probe(1);
  else if (!strcmp(mode, "ubo-multi") || !strcmp(mode, "ubo-multi-validation"))
    rc = ubo_multi_probe(strstr(mode, "validation") != NULL);
  else if (!strcmp(mode, "ubo-multi-good") || !strcmp(mode, "ubo-multi-bad"))
    rc = ubo_multi_draw(!strcmp(mode, "ubo-multi-bad"), 0);
  else if (!strcmp(mode, "ubo-dynamic-good") || !strcmp(mode, "ubo-dynamic-bad"))
    rc = ubo_dynamic_draw(!strcmp(mode, "ubo-dynamic-bad"));
  else if (!strncmp(mode, "bc-images", 9))
    rc = bc_images_probe(strstr(mode, "validation") != NULL,
        strstr(mode, "linked") ? 3 : strstr(mode, "dlsym") ? 2 : strstr(mode, "gdpa") ? 1 : 0);
  else if (!strcmp(mode, "bc-decode") || !strcmp(mode, "bc-decode-validation"))
    rc = bc_decode_probe(strstr(mode, "validation") != NULL);
  else if (!strcmp(mode, "memory-ranges") || !strcmp(mode, "memory-ranges-validation"))
    rc = memory_ranges_probe(strstr(mode, "validation") != NULL);
  else if (!strcmp(mode, "blender-readback") || !strcmp(mode, "blender-readback-validation"))
    rc = memory_readback_probe(strstr(mode, "validation") != NULL);
  else if (!strncmp(mode, "timeline-", 9)) {
    int khr = strstr(mode, "khr") != NULL;
    int route = strstr(mode, "linked") ? 3 : strstr(mode, "elf") ? 2 : strstr(mode, "gdpa") ? 1 : 0;
    rc = timeline_probe(khr, route, strstr(mode, "validation") != NULL, strstr(mode, "queues") != NULL);
  }
  else if (!strcmp(mode, "command-alloc") || !strcmp(mode, "command-alloc-blender"))
    rc = command_allocator_probe(!strcmp(mode, "command-alloc-blender"));
  else if (!strncmp(mode, "render-segments", 15))
    rc = rendering_segments_probe(strstr(mode, "validation") != NULL,
      strstr(mode, "gdpa") ? 1 : strstr(mode, "elf") ? 2 : 0, strstr(mode, "control") == NULL);
  else if (!strcmp(mode, "render-owners") || !strcmp(mode, "render-owners-blender"))
    rc = render_owners_probe(!strcmp(mode, "render-owners-blender"));
  else if (!strcmp(mode, "render-core13") || !strcmp(mode, "render-khr13"))
    rc = ubo_render_probe(!strcmp(mode, "render-core13") ? 1 : 2, -1, 0);
  else if (!strcmp(mode, "render-core13-elf") || !strcmp(mode, "render-khr13-elf"))
    rc = ubo_render_probe(!strcmp(mode, "render-core13-elf") ? 1 : 2, 2, 0);
  else if (!strcmp(mode, "render-core13-linked") || !strcmp(mode, "render-khr13-linked"))
    rc = ubo_render_probe(!strcmp(mode, "render-core13-linked") ? 1 : 2, 3, 0);
  else if (!strcmp(mode, "render-core13-validation") || !strcmp(mode, "render-khr13-validation"))
    rc = ubo_render_probe(!strcmp(mode, "render-core13-validation") ? 1 : 2, -1, 1);
  else if (!strcmp(mode, "ubo-dynamic"))
    rc = ubo_dynamic_probe(0);
  else if (!strcmp(mode, "ubo-dynamic-validation"))
    rc = ubo_dynamic_probe(1);
  else if (!strcmp(mode, "ubo-validation"))
    rc = ubo_validation_probe();
  else if (!strcmp(mode, "validation"))
    rc = validation_probe();
  else if (!strcmp(mode, "ubo"))
    rc = ubo_probe();
  else if (!strcmp(mode, "ubo-good") || !strcmp(mode, "ubo-bad"))
    rc = ubo_draw(!strcmp(mode, "ubo-bad"), 0);
  else if (!strcmp(mode, "vk"))
#ifdef HYBRIS_PROBE_LINKED
    rc = vkprobe("linked");
#else
    rc = vkprobe("gipa");
#endif
  else if (!strcmp(mode, "vk-dlsym"))
    rc = vkprobe("dlsym");
  else if (!strcmp(mode, "vk-gdpa"))
    rc = vkprobe("gdpa");
  else if (!strcmp(mode, "vk-core11"))
    rc = vkprobe("core11");
  else if (!strcmp(mode, "vk-khr11"))
    rc = vkprobe("khr11");
  else if (!strcmp(mode, "0") || !strcmp(mode, "2") || !strcmp(mode, "3"))
    rc = eglprobe(atoi(mode));
  else {
    fprintf(stderr, "Unknown probe mode: %s\n", mode);
    rc = 2;
  }
  probe_mappings("complete");
  printf("RESULT %d\n", rc);
  return rc;
}
