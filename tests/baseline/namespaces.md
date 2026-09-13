# Android linker namespaces

The Q loader reads Android 11+'s boot-generated `/linkerconfig/ld.config.txt`.
It retains `/usr/lib/hybris-config/ld.config.txt` as a fallback for hosts that
expose a copy. Compiled flat search paths apply only when configuration is
unavailable; an explicit `HYBRIS_LD_LIBRARY_PATH`, including an empty value,
retains its existing override semantics.

Previously, only the copied configuration was considered, and the compiled
vendor-first path overrode configured platform paths. On OnePlus PJZ110
(Android 16, Adreno 830), system Vulkan/EGL dependencies resolved to vendor
copies of `libziparchive.so` and `libaconfig_storage_read_api_cc.so` whose
SELinux labels prohibit the requested mapping. Loading Android's actual
configuration restores separate platform and HAL dependency resolution.
System copies are then selected. No mapping permission or SELinux policy is
changed, and no vendor library is copied to evade a denial.

Run after building the library and baseline probes:

```sh
python3 tests/baseline/run.py --serial DEVICE \
  --case hybris-namespaces --case native-vk --case hybris-vk \
  --case native-2 --case hybris-2 --case native-3 --case hybris-3 \
  --case hybris-tls-mrs
```

`hybris-namespaces` requires distinct exported `default` and `sphal`
namespaces and platform default paths containing `/system/lib64` without
`/vendor/lib64`. It is intended for modern 64-bit Android devices exporting
those namespaces, not legacy devices without namespace configuration.

## Verified 2026-09-13

[Saved evidence](namespace-verified-2026-09-13.json) records passing namespace,
Vulkan, GLES 2/3 and TLS checks on Redmi M2012K11AC, vivo X300, OnePlus 8T and
OnePlus PJZ110. PJZ110 native controls passed before the loader change; its
three failing hybris graphics cases now pass. The previous loader fails the
new namespace probe (`default=missing sphal=missing`). The integrity suite
also passes all 18 cases on each device, with unchanged on-disk inputs.

On PJZ110, a standalone probe and the rebuilt libraries staged under the
Omarchy APK's UID also pass Vulkan, GLES 2/3 and namespaces using `run-as`.
This tests application-domain library access; it does not test Wayland
presentation or certify the APK's separate Turnip desktop route.
