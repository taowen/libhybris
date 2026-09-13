#include "probe.h"

/* Modern Android exports separate platform and same-process HAL namespaces.
 * Losing the generated config silently collapses them into a flat search path. */
int namespaces_probe(void) {
  void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!common) return 2;
  void *(*get_namespace)(const char *) = dlsym(common, "hybris_get_exported_namespace");
  void (*get_paths)(char *, size_t) = dlsym(common, "hybris_get_LD_LIBRARY_PATH");
  if (!get_namespace || !get_paths) return 2;
  void *platform = get_namespace("default");
  void *sphal = get_namespace("sphal");
  printf("NAMESPACE default=%s sphal=%s distinct=%d\n",
      platform ? "present" : "missing", sphal ? "present" : "missing",
      platform && sphal && platform != sphal);
  if (!platform || !sphal || platform == sphal) return 2;
  char paths[16384] = {0};
  get_paths(paths, sizeof(paths));
  printf("NAMESPACE platform_search=%s\n", paths);
  /* A vendor HAL search directory must not be injected into the platform
   * namespace's configured default paths. Explicit environment overrides are
   * separate and remain supported by the loader. */
  if (!strstr(paths, "/system/lib64") || strstr(paths, "/vendor/lib64")) return 2;
  return 0;
}
