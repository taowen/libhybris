# Window integration tests

One entry point runs Wayland, XCB and Xlib clients against an installed,
already running debuggable compositor APK. Xwayland and its TAWC-DRI patches
belong to Ardesk or the external APK; android_wlegl belongs to anlabwc.
libhybris builds and deploys only its bridge libraries and probe clients.
The runner does not install, start, restart or stop the compositor or X server.

## Build clients and prepare the external service

```sh
tools/build-aarch64.sh
tests/wsi/build.sh
python3 tests/x11/build.py
```

Install/start the compositor with its owning project's tooling. See the
[external service contract](compositor/README.md). The default package is
`io.taowen.hybriswsitest`; `--package` selects another installed debuggable
compositor package. The default Wayland socket is `files/runtime/wayland-0`. Use `--runtime-dir`
and `--wayland-display` for the endpoints supplied by the APK; an absolute
Wayland socket path is also accepted. X11-only checks do not require a Wayland
socket at that default path.
The compositor/desktop must supply an existing local X display for X11 tests;
`--display :0` is the default, and `--xauthority` supplies an optional device
path to its authentication file. Missing services are errors, with no private
server fallback. Host tools are adb, Python 3, Pillow/LittleCMS, the pinned
cross-builder and the Android NDK for the client watchdog.

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
| XCB/Xlib | `missing-protocol` | Support false and eight surface rejections; requires an existing externally managed display without TAWC-DRI |
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
Each client directory contains its result, client input manifests, logs,
mappings, Android library hashes, readbacks and screenshots. APK identity and
compositor PID/start time are recorded and checked; X11 records the selected
external display and obtains protocol evidence from the connected server.
The runner holds a per-device lock and cleans up only its own deployed clients.
It neither inspects a prescribed server asset layout nor deploys server ELFs.
Existing display placement must satisfy the screenshot checks; historical
private-server screenshots are not evidence for this workflow.

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
[previous consolidation results](integration-review.md) preserve historical results. See [external-service validation](external-service-review.md)
for the current boundary and its remaining coverage. Rootless/multiwindow X11, minimize,
disconnect recovery, long-running FD accounting, X11 capture and CTS remain
open. Headless baseline and desktop-GL application tests are separate suites.
