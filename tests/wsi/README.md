# Window integration tests

One entry point runs Wayland, XCB and Xlib clients against an installed,
already running debuggable compositor APK. Xwayland and its TAWC-DRI patches
belong to Ardesk or the external APK; android_wlegl belongs to anlabwc.
The same clients select either the hybris ICD or product Mesa/Turnip through
the standard Vulkan loader. libhybris builds and deploys only client artifacts.
The runner does not install, start, restart or stop the compositor or X server.

## Build clients and prepare the external service

```sh
tools/build-aarch64.sh
tests/wsi/build.sh
python3 tests/x11/build.py
```

Install/start the compositor with its owning project's tooling. See the
[external service contract](compositor/README.md). The default package is
`io.taowen.ardesk`; `--package` selects another installed debuggable
compositor package. The default Wayland socket is `files/runtime/wayland-0`. Use `--runtime-dir`
and `--wayland-display` for the endpoints supplied by the APK; an absolute
Wayland socket path is also accepted. Hybris X11 checks do not require a Wayland
socket at that default path; product Turnip also uses Wayland for AHB allocation,
including during XCB/Xlib runs.
The compositor/desktop must supply an existing local X display for X11 tests;
`--display :1` is the default, and `--xauthority` supplies an optional device
path to its authentication file. Missing services are errors, with no private
server fallback. Host tools are adb, Python 3, Pillow/LittleCMS, the pinned
cross-builder and the Android NDK for the client watchdog.

X11 probes center their unmanaged window so Android system bars and the
initial pointer do not cover it. Screenshot checks still require every expected
window pixel. Wayland presentation cycles the advertised RGBA/BGRA UNORM
formats across resize epochs and requests the advertised sampled/color
attachment usages as well as the required transfer usages.

## Run one selected check

```sh
python3 tests/wsi/run.py --serial SERIAL --platform xcb --case resize \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/glibc/libvulkan.so.1 \
  --validation-layer /path/to/libVkLayer_khronos_validation.so \
  --validation-manifest /path/to/VkLayer_khronos_validation.json
```

Use `--platform xlib` or `--platform wayland`. For the tested Mali firmware use
`vulkan.mali.so`; the known-build MMUD quirk is documented in the ICD README.
The default `--backend hybris` requires `--icd-hal` and `--vulkan-loader` for
Vulkan cases. `--backend turnip` instead uses the verified product runtime
from `--mesa-build` (defaults to `tests/desktop-gl/build`). Build it using
`tests/desktop-gl/build.sh`, which invokes the parent product Mesa build.
Turnip rejects vendor HAL/quirk arguments. The frontend window implementation
has been deleted. XCB/Xlib `control` remains a non-Vulkan environment check.

```sh
python3 tests/wsi/run.py --serial SERIAL --backend turnip --platform wayland \
  --validation-layer /path/to/libVkLayer_khronos_validation.so \
  --validation-manifest /path/to/VkLayer_khronos_validation.json
```

Backend staging, version discovery, pixel/screenshot checks and layer/capture
handling are shared. Each record identifies its selected backend and retains
input manifests and staged loader/ICD hashes. The Turnip path keeps the
product loader/libc pair; X11 client dependencies cannot replace that pair.
Other client/backend SONAME conflicts still fail. `swapchain-review` remains
hybris-only because it exercises that adapter's allocation hooks.

| Platform | Case | Evidence |
| --- | --- | --- |
| Wayland | `present` | 24 GPU readbacks, three sizes, six physical screenshots, surface lifetimes |
| Wayland | `swapchain-review` | Present plus allocator, timeout, retirement, old images and multi-present boundaries |
| XCB/Xlib | `present` | Eight GPU readbacks, two physical screenshots, TAWC-DRI release before reuse |
| XCB/Xlib | `resize` | 24 readbacks, three sizes, six screenshots, acquire/present out-of-date, semaphore reuse and swapchain replacement |
| XCB/Xlib | `surface-lost` | Destroy the native window while holding an image; surface/acquire/present errors, unchanged acquire index/fence and present semaphore reuse |
| XCB/Xlib | `acquire-timeout` | Zero/finite acquire timeout with unchanged index and unsignaled fence |
| XCB/Xlib | `missing-protocol` | Support false and eight surface rejections; requires an existing externally managed display without TAWC-DRI |
| XCB/Xlib | `control` | XCB create/map/clear/GetImage environment check; no Vulkan or GPU claim |

For XCB/Xlib `present` or `resize`, `--surface-format N` selects one advertised
VkFormat: 37 = RGBA8 UNORM, 44 = BGRA8 UNORM, 43 = RGBA8 SRGB, 50 = BGRA8 SRGB.
The client logs the full advertised list and the selected format; the host
checks the selection. An absent or unhandled format reports UNSUPPORTED.
These red/green endpoint frames verify allocation, transfer, readback and
presentation, not the complete SRGB transfer function or nontrivial alpha.

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
always collected with a per-owner bound. Every successful Vulkan case, including
negative cases, must retain the selected loader/ICD mappings and, when requested,
the validation layer mapping plus its zero-error verdict.

Current cross-backend device results: [product window gates](product-backends.md).

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

The [frontend deletion record](frontend-removal.md) lists the removed code,
final build checks, vendor-ICD window regressions and the retained XCB resize
validation failure with pre-deletion controls.

## Application window gates

The parent Ardesk tests reuse this Host for the per-device lock, compositor
identity, bounded execution, screenshots and cleanup:

- `tests/test-scene-ahb-device.py` — native AHB/TAWC-DRI scene pixels. Not a
  Vulkan client; no VVL or GFXReconstruct claim.
- `tests/test-teapot-device.py` — product GLX and Wayland EGL teapots. Checks
  a frozen still frame and a resized frame on the physical screen. Both
  application gates share `tests/device_gate.py` plus `Host.deploy`/`pull_maps`.
  VVL and capture stay on the Vulkan probes above rather than a second replay
  path. The old `guest-desk` + `TEAPOT_ONCE` layout script is gone.

`desktop-gl` remains an offscreen Zink workload and does not substitute for
either window gate. See [application evidence](product-backends.md#native-scene-shared-host--2026-09-08).
