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
  if (!strcmp(mode, "egl-life"))
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
  else if (!strcmp(mode, "ubo-staged"))
    rc = ubo_staged_probe(0);
  else if (!strcmp(mode, "ubo-staged-validation"))
    rc = ubo_staged_probe(1);
  else if (!strcmp(mode, "ubo-large"))
    rc = ubo_large_probe(0);
  else if (!strcmp(mode, "ubo-large-validation"))
    rc = ubo_large_probe(1);
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
