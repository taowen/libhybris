# Window integration tests

One entry point runs Wayland, XCB and Xlib against the disposable
`io.taowen.hybriswsitest` APK. The APK contains **anlabwc, TAWC-DRI Xwayland,
their Android libraries and xkb assets**. The runner starts that APK, deploys
the selected glibc probe and libhybris, checks the results, then stops it.
It never launches the Ardesk desktop or accepts an arbitrary application UID.

## Build and install

Build the client libraries and both platform probes, then build the test APK:

```sh
tools/build-aarch64.sh
tests/wsi/build.sh
python3 tests/x11/build.py \
  --xwayland-source /path/to/ardesk/third_party/xwayland \
  --ndk-prefix /path/to/ardesk/build/ndk-prefix
python3 tests/wsi/compositor/build.py \
  --backend-apk /path/to/ardesk-debug.apk \
  --backend-library /path/to/libanlabwc.so \
  --x11-build tests/x11/build
/path/to/ardesk/tools/install-apk.sh --serial SERIAL \
  tests/wsi/build/compositor/hybris-wsi-test.apk
```

`--backend-library` is optional: otherwise libanlabwc is taken from the supplied
APK. This APK is a build input for native dependencies/assets; it is not run or
installed by the test. The builder records its hash, selected backend hash,
transitive libraries and the included Xwayland build manifest. See
[APK build details](compositor/README.md) and [Xwayland inputs/protocol](../x11/README.md).

Rebuild the test APK only when changing its native server bundle or host.
Rebuilding libhybris or a glibc probe does not require reinstalling the APK.
Host tools include adb, Python 3, Pillow/LittleCMS, the pinned cross-builder,
Meson/Ninja, patchelf and Android SDK/NDK.

## Run one selected check

```sh
python3 tests/wsi/run.py --serial SERIAL --platform xcb --case resize \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/glibc/libvulkan.so.1 \
  --validation-layer /path/to/libVkLayer_khronos_validation.so \
  --validation-manifest /path/to/VkLayer_khronos_validation.json
```

Use `--platform xlib` or `--platform wayland`. For the tested Mali firmware use
`vulkan.mali.so` and explicit `--icd-mali-loader-quirk`; its scope is documented
in the ICD README. Omitting HAL/loader arguments selects the replacement
libvulkan frontend for Wayland. XCB/Xlib Vulkan checks require the standard ICD.

| Platform | Case | Evidence |
| --- | --- | --- |
| Wayland | `present` | 24 GPU readbacks, three sizes, six physical screenshots, surface lifetimes |
| Wayland | `swapchain-review` | Present plus allocator, timeout, retirement, old images and multi-present boundaries |
| XCB/Xlib | `present` | Eight GPU readbacks, two physical screenshots, TAWC-DRI release before reuse |
| XCB/Xlib | `resize` | 24 readbacks, three sizes, six screenshots, acquire/present out-of-date, semaphore reuse and swapchain replacement |
| XCB/Xlib | `acquire-timeout` | Zero/finite acquire timeout with unchanged index and unsignaled fence |
| XCB/Xlib | `missing-protocol` | Support false and eight surface rejections; requires explicit `--server-binary /path/to/unextended/Xwayland` |
| XCB/Xlib | `control` | XCB create/map/clear/GetImage environment check; no Vulkan or GPU claim |

`--repeat N` runs sequential clients under the same compositor identity and
records compositor FD snapshots before/after each. It does not itself assert
that all resource leaks are absent. `--build`, `--probe` and `--out` select
local library/probe/result directories. `--timeout` bounds the host watchdog;
X11 also has a 25-second native supervisor deadline.

Validation requires both layer arguments and enables SyncVal. Capture uses
`--platform wayland --case present --capture-tools /path/to/gfxreconstruct/install`
in a separate invocation. The pinned capture tool cannot combine with VVL or
the allocator-failure workload. Capture retains file identity, metadata,
conversion and virtual-swapchain replay checks from the previous runner.
`--trace` enables compiled Wayland tracepoints; build with
`tools/build-aarch64.sh --debug --incremental` first. X11 protocol serials are
always collected with a per-owner bound.

## Evidence and implementation

Every invocation writes `build/results/<run>/result.json` plus `isolation.json`.
Each client directory contains its result, input manifests, logs, mappings,
Android library hashes, readbacks and screenshots. The runner records APK identity; X11 runs also verify the actual extracted
Xwayland/dependency hashes. It holds a per-device lock, checks
PID plus process start time, and reports cleanup failures. The default Xwayland
is executed from APK assets extracted into its private directory. An explicit
`--server-binary` override is hashed; its dependencies still come from the APK.
There is no implicit fallback to a host-built server.

Both probes keep screenshot frames still for two seconds. The shared collector
waits 0.6 seconds after the marker before sampling the physical screen. It
still requires the complete expected color transition and dimensions; this is
not proof of presentation completion timing or a performance measurement.

`run.py` owns argument validation and orchestration; `host.py` owns device
lifecycle, transport and screenshots. `wayland.py` and `x11.py` contain platform
staging and evidence checks, with no standalone CLI. The obsolete compositor
wrapper, X11 runner and surface-only binary have been removed. Its extension
and FIFO checks now execute in the actual Wayland presentation probe.

[Historical evidence](history.md), [swapchain review](swapchain-review.md),
[validation/capture review](validation-capture-review.md) and
[current consolidation results](integration-review.md) preserve the distinction
between old and current test paths. Rootless/multiwindow X11, minimize,
disconnect recovery, long-running FD accounting, X11 capture and CTS remain
open. Headless baseline and desktop-GL application tests are separate suites.
