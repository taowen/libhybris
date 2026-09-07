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
  else if (!strcmp(mode, "caps2"))
    rc = caps2_probe();
  else if (!strcmp(mode, "caps"))
    rc = caps_probe(0);
  else if (!strcmp(mode, "wsi-disabled"))
    rc = caps_probe(1);
  else if (!strncmp(mode, "scaled-vertex", 13))
    rc = scaled_vertex_probe(strstr(mode, "validation") != NULL,
      strstr(mode, "gdpa") ? 1 : strstr(mode, "elf") ? 2 : strstr(mode, "linked") ? 3 : 0);
  else if (!strcmp(mode, "ubo-template"))
    rc = ubo_template_probe(0);
  else if (!strcmp(mode, "ubo-template-validation"))
    rc = ubo_template_probe(1);
  else if (!strcmp(mode, "ubo-staged"))
    rc = ubo_staged_probe(0);
  else if (!strcmp(mode, "ubo-staged-validation"))
    rc = ubo_staged_probe(1);
  else if (!strcmp(mode, "ubo-large"))
    rc = ubo_large_probe(0);
  else if (!strcmp(mode, "ubo-large-validation"))
    rc = ubo_large_probe(1);
  else if (!strcmp(mode, "ubo-dynamic-good") || !strcmp(mode, "ubo-dynamic-bad"))
    rc = ubo_dynamic_draw(!strcmp(mode, "ubo-dynamic-bad"));
  else if (!strcmp(mode, "memory-ranges") || !strcmp(mode, "memory-ranges-validation"))
    rc = memory_ranges_probe(strstr(mode, "validation") != NULL);
  else if (!strncmp(mode, "timeline-", 9)) {
    int khr = strstr(mode, "khr") != NULL;
    int route = strstr(mode, "linked") ? 3 : strstr(mode, "elf") ? 2 : strstr(mode, "gdpa") ? 1 : 0;
    rc = timeline_probe(khr, route, strstr(mode, "validation") != NULL, strstr(mode, "queues") != NULL);
  }
  else if (!strcmp(mode, "command-alloc"))
    rc = command_allocator_probe();
  else if (!strcmp(mode, "render-owners"))
    rc = render_owners_probe();
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
