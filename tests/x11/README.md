# X11 Vulkan developer probe

This independent tool builds a glibc Vulkan client, a small Android process
supervisor and a private TAWC-DRI Xwayland. It uses the disposable
`io.taowen.hybriswsitest` compositor APK. Each invocation selects one check;
it does not launch the Ardesk desktop or a rootfs. No Mesa changes are needed.
The existing anlabwc android_wlegl backend accepts the imported buffers.

## Build and run

First build the libhybris baseline (`tests/baseline/build.sh`, see its README)
and install the [disposable compositor](../wsi/compositor/README.md).
Supply an Android Xwayland source checkout and an existing NDK dependency
prefix containing `android-cross.ini`, `native.ini`, headers, libraries and
pkg-config files. The builder reads these inputs, builds an isolated source
copy under ignored `tests/x11/build`, and never installs into that prefix.
The tested base is Ardesk's Android-patched Xwayland 24.1.6, commit
`6372702c0c12461787d9fab351912b9663dbcea4`.
Host requirements include Meson/Ninja, Git, tar, patchelf, the baseline pinned
cross-builder container and Android NDK 29.0.14206865.

```sh
python3 tests/x11/build.py \
  --xwayland-source /path/to/ardesk/third_party/xwayland \
  --ndk-prefix /path/to/ardesk/build/ndk-prefix
python3 tests/x11/run.py --serial SERIAL --api xcb --case present \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/glibc/libvulkan.so.1
```

Use `--api xlib` to exercise XOpenDisplay and VkCreateXlibSurfaceKHR. On the
tested Mali device use `vulkan.mali.so` and explicit
`--icd-mali-loader-quirk` (see the ICD README for the firmware-specific scope).
Add both `--validation-layer /path/to/libVkLayer_khronos_validation.so` and
`--validation-manifest /path/to/VkLayer_khronos_validation.json` for VVL and
SyncVal. The runner verifies the enabled layer is actually mapped and requires
zero validation errors through instance destruction.

| Case | Check |
| --- | --- |
| `control` | XCB map/clear/GetImage; environment control, no Vulkan evidence and no loader/HAL arguments required |
| `present` | Standard loader → ICD → vendor HAL, eight alternating GPU clears, full 320×240 image readbacks, two matching physical screenshots, matched protocol release before buffer reuse |
| `missing-protocol` | Use `--server-binary /path/to/unextended/Xwayland`; presentation support must be false and eight surface creations must reject with VK_ERROR_UNKNOWN |
| `acquire-timeout` | Hold all three images, require zero-time NOT_READY and finite 20 ms TIMEOUT, unchanged output index and unsignaled fence |

The supervisor inherits a private Unix listening FD. Xlib uses an additional
private abstract Unix socket; no filesystem `/tmp` or desktop display is
required. It supervises server/client process groups with a 25-second deadline.
The runner uses the same per-device lock as the Wayland fixture, checks
compositor PID plus start time, collects evidence and stops the test package.
The client selects only its tested platform extension and checks that the
other platform's surface entry point is unavailable through instance lookup.

`build/manifest.json` records source/compiler, Xwayland revision, protocol patch,
dependency and ELF hashes. `build/results/<run>/result.json` records staged
inputs, actual loader/HAL mappings, device identity, isolation/cleanup and exit
status. Logs, protocol serials, readbacks and screenshots stay beside it.
Explicit alternate servers are separately hashed. Present screenshots include
two one-second pauses, so elapsed time is developer feedback, not performance.

## Protocol and ownership

The ICD requires local AF_UNIX, a supported TrueColor visual and TAWC-DRI 0.3.
It sends gralloc native handles via PresentBuffer/SCM_RIGHTS and waits for
BufferRelease on its own XCB special-event queue. It does not take application
X event ownership or use PRESENT_SOCKET. Xlib obtains its XCB connection through
XGetXCBConnection. Pending presentations retain native buffer references even
when a swapchain pool is retired; imported Vulkan images also retain theirs.
The shared swapchain layer delegates configure/dequeue/queue/cancel/disconnect
to separate Wayland and X11 owners. Release fences are consumed with a host
wait before submission, matching the existing android_wlegl limitation.
`HYBRIS_X11_TRACE=1` enables at most 64 present/release records per owner.

The vendored `patches/tawc-dri.patch` is copied without modification from
[wmww/tawc](https://github.com/wmww/tawc), commit
`4d74c2db0c9cc927118d493eba32107d7e8971d5`, file
`deps/xwayland-patches/xwayland/02-tawc-step3-ahb-present.patch`.
Its source copyright and MIT license notices are retained. Patch SHA256:
`eba1d87a41943dedb958c27e660ce1f7e1e05cd47d222fde32d366995bc61a46`.
The Xwayland builder disables GLX, glamor, DRM, DRI3 and MIT-SHM; this fixture
exercises native-buffer transport rather than a GL renderer.

## Device evidence, 2026-09-08

All twelve final runs below passed with VVL/SyncVal, stable compositor identity
and empty cleanup errors. Each present run has eight exact GPU image readbacks,
eight protocol presents, seven observed releases and two matching screenshots.

| Device | API | Present | Missing protocol | Acquire timeout |
| --- | --- | --- | --- | --- |
| OnePlus 8T / Adreno | XCB | 20260908T064309-f2149cea | 20260908T064325-a63ec5ab | 20260908T064339-ff13069a |
| OnePlus 8T / Adreno | Xlib | 20260908T064349-a4d95330 | 20260908T064402-92e4bba8 | 20260908T064413-9ceac303 |
| X300 / Mali | XCB | 20260908T064309-430aeca5 | 20260908T064316-d3826e2f | 20260908T064320-a3d63cbc |
| X300 / Mali | Xlib | 20260908T064324-b8ff163b | 20260908T064331-f01225e6 | 20260908T064335-22c9f8de |

Present runs took 7.1 seconds on USB Mali and 12.9–16.2 seconds on wireless
OnePlus; all final cases took 3.8–20.8 seconds including staging and cleanup.
An earlier OnePlus Xlib run `20260908T063020-42610e25` rendered correctly but
failed isolation because startup briefly returned two compositor PIDs. It
remains a FAIL; the runner now waits for one stable PID/start time before use.

The same production build passed both devices' full Wayland swapchain review
with VVL/SyncVal (24 frames, three sizes, surface lifetime, allocator, retirement,
multi-present and timeout checks): isolated runs
`20260908T063331-e8d289ce` / `20260908T063331-29bd303a`.
Headless version, GDPA, lifetime and direct allocator cases also passed:
`20260908T063909-f283b33b` / `20260908T063909-2c7fd235`.

This establishes rootful, single-window, fixed-size XCB/Xlib presentation on
two vendor drivers. Rootless placement, multiple windows, resize/out-of-date,
minimize, disconnect recovery, long-running FD accounting, X11 capture/replay
and CTS remain unverified or unimplemented. The replacement-libvulkan frontend
has no new X11 WSI here. G11 remains open.
