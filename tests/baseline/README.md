# Headless GPU baseline

One C probe, built for glibc and bionic, run on the same Android device.
No APK, root, rootfs, compositor, X server, or CTS download is needed at runtime.
This is a smoke test, not conformance certification or application compatibility coverage.

## Build

From this repository (not the parent ardesk tree):

```sh
bash ../../tools/build-aarch64.sh
bash build.sh
python3 run.py --serial 29854870
```

`tools/build-aarch64.sh` cross-compiles this checkout, stages `build/install`
and `build/runtime`, and writes `build/manifest.json` with ELF sha256/build-id
for every installed binary. It refuses `vulkanplatform_x11.so`; that plugin is
not a current source target. The AArch64 toolchain image comes from the parent
project's `tools/ensure-glibc-builder.sh` when this tree is checked out as
`third_party/libhybris`. Otherwise set `BUILDER_IMAGE` and `--headers`.

`build.sh` compiles `probe-glibc`, `probe-glibc-linked` (DT_NEEDED libvulkan)
and `probe-bionic`. Set `BIONIC_CC` if the default NDK clang wrapper is absent.

```sh
python3 run.py --serial 29854870 \
  --hybris-lib build/install/usr/lib/hybris \
  --runtime build/runtime \
  --manifest build/manifest.json
```

The runner uses a unique `/data/local/tmp/libhybris-baseline-<run-id>` directory,
records loaded ELF hashes, rejects unknown platform plugins, and kills leftover
device PIDs after a host timeout. Results go under `build/results/<run-id>/`.
Each probe arms `alarm(25)` from a constructor so a stuck loader still dies.
Exit 0 means the implemented checks passed, 3 means unsupported, 124 timeout;
the runner returns nonzero for FAIL/TIMEOUT/CRASH.

## Coverage

- Vulkan: enumerate extensions and device features; create instance/device/queue;
  allocate and bind memory; submit a GPU buffer fill; wait on a fence; verify
  all 1024 returned words. Instance requests Vulkan 1.0; reported device version
  does not imply that later-version features were exercised.
- Dispatch: resolve the same symbols via link, dlsym, GIPA and GDPA; a missing
  name must return NULL instead of a trampoline. BeginRendering/Submit2 are
  reported present or null according to the loaded driver, not advertised.
- EGL/GLES: pbuffer contexts requesting ES 2 and ES 3; clear and read back;
  compile/link a simple shader pair, draw a triangle using a VBO, verify a pixel.
  Drivers may return a higher compatible context version.
- Desktop GL: determine whether the native EGL backend offers a desktop GL
  config/context. This does not test Zink or another translation layer.

No window surfaces, AHB sharing, swapchains, shader stress, threading, texture
formats or full application workloads are covered. EGL/Vulkan use the null
platform; advertised platform extensions do not prove X11/Wayland WSI works.
Readbacks are test assertions, not a proposed production presentation path.

## First device result: 29854870

Android 13 (SDK 33), model M2012K11AC. Vendor driver identifies itself as
Adreno 650, Vulkan 1.1.128, driver `0x801f6000`; GLES 3.2 V@0502.0,
GLSL ES 3.20, EGL 1.5. Use queried driver identity rather than inferred SoC.
Source examined/rebuilt: `dcc3588`.

| Case | Android native | glibc + hybris | glibc linked |
|---|---|---|---|
| Vulkan GPU fill + fence + readback | PASS | PASS | PASS |
| Dispatch (link/dlsym/GIPA/GDPA) | PASS | PASS | PASS |
| GLES context request 2, clear + shader draw + readback | PASS | PASS | — |
| GLES context request 3, clear + shader draw + readback | PASS | PASS | — |
| Native desktop GL context | unsupported | unsupported | — |

On this run `vkCmdBeginRenderingKHR` and `vkQueueSubmit2KHR` returned NULL;
`vkCmdBeginRendering` was present. That is a driver export observation, not a
Vulkan 1.3 claim. Both Vulkan paths report BC=false, ETC2=true, ASTC=true,
geometry/tessellation=true, shaderFloat64=false, shaderInt64=false.
These are capability queries, not tests of those features. Neither native nor
hybris advertises desktop OpenGL through EGL.

The 2026-09-06 rerun used `tools/build-aarch64.sh` from this checkout. Results
are under `build/results/<run-id>/` with `manifest.json` ELF hashes. Rebuild
the library when measuring a different source revision; a dirty working tree
is recorded as `source_dirty` in the manifest.
