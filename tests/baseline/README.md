# Headless GPU baseline

One C probe, built for glibc and bionic, run on the same Android device.
No APK, root, rootfs, compositor, X server, or CTS download is needed at runtime.
This is a smoke test, not conformance certification or application compatibility coverage.

## Build

From this repository root:

```sh
bash tools/build-aarch64.sh
export ANDROID_NDK_HOME=/path/to/android-ndk
bash tests/baseline/build.sh
python3 tests/baseline/run.py --serial 29854870
```

`tools/build-aarch64.sh` cross-compiles this checkout, stages `build/install`
and `build/runtime`, and writes `build/manifest.json` with ELF sha256/build-id
for every installed binary. It refuses `vulkanplatform_x11.so`; that plugin is
not a current source target. Each build starts with fresh source/install/runtime staging. The AArch64 toolchain image comes from the parent
project's `tools/ensure-glibc-builder.sh` when this tree is checked out as
`third_party/libhybris`. Otherwise set `BUILDER_IMAGE` and `--headers`.

`tests/baseline/build.sh` compiles `probe-glibc`, `probe-glibc-linked` (DT_NEEDED libvulkan)
and `probe-bionic`. Set `ANDROID_NDK_HOME` or an explicit `BIONIC_CC`.

```sh
python3 tests/baseline/run.py --serial 29854870 \
  --hybris-lib /path/to/install/usr/lib/hybris \
  --runtime /path/to/runtime \
  --manifest /path/to/manifest.json
```

The runner uses a unique `/data/local/tmp/libhybris-baseline-<run-id>` directory,
verifies the supplied manifest against all staged ELF files (including SONAME
aliases), rejects unknown platform plugins, and kills its recorded probe PID
after a host timeout after checking its executable path. Manifest hashes describe
staged files, not observed runtime mappings. Custom library paths do not inherit
the default manifest; supply their matching manifest explicitly. Results go under `build/results/<run-id>/`.
Each probe arms `alarm(25)` from its own constructor. A dependency constructor
can run earlier, so the host timeout and pre-exec PID tracking are still required.
Exit 0 means the implemented checks passed, 3 means unsupported, 124 timeout;
the runner returns nonzero for FAIL/TIMEOUT/CRASH.

## Coverage

- Vulkan: enumerate extensions and device features; create instance/device/queue;
  allocate and bind memory; submit a GPU buffer fill; wait on a fence; verify
  all 1024 returned words. Instance requests Vulkan 1.0; reported device version
  does not imply that later-version features were exercised.
- Dispatch: resolve the same symbols via link, dlsym, GIPA and GDPA; a missing
  name must return NULL. Negative checks cover NULL-instance non-global queries,
  GDPA instance/physical-device commands and disabled device extensions.
  Ordinary proc queries preserve backend resolution; only WSI/frontend commands
  substitute local wrappers. Equal function addresses are not required.
  This is not yet a generated per-device compatibility dispatch layer.
- Life: two devices from one instance, destroy/recreate, second `dlopen`,
  and two threads creating/destroying devices and fences. This is not a
  generation-tagged object table. A separate `unload` case closes the final
  library reference; process exit must also succeed.
- Caps: print limits and advertised features; reject enabling an
  unadvertised feature or unknown extension with the exact Vulkan error.
  A disabled device extension must not be exposed through GDPA. This does
  not compare separate native/effective capability sets or exercise feature chains.
- UBO: 272-byte widget-shaped std140 block (parameters@0, MVP@192,
  checker@256, srgb@268), 12 vertices / 18 indices, `gl_VertexIndex`.
  The fragment shader encodes those fields into the pixel. A second
  pass injects a different UBO binding and must produce the expected different
  pixel. This is a negative control, not automatic first-failing-draw diagnosis.
  It keeps an identity MVP so geometry still covers the sampled pixel.
  Render-pass writes are synchronized with transfer reads, and readback writes
  with host reads.
- Standard validation/capture integration remains unimplemented. The former
  custom layer chain was removed: it rewrote dispatchable handles and stripped
  creation chains/extensions, so its success did not validate the real path.
- EGL/GLES: pbuffer contexts requesting ES 2 and ES 3; clear and read back;
  compile/link a simple shader pair, draw a triangle using a VBO, verify a pixel.
  Drivers may return a higher compatible context version.
- Desktop GL: determine whether the native EGL backend offers a desktop GL
  config/context. This does not test Zink or another translation layer.

No window surfaces, AHB sharing, swapchains, shader stress, GLES multithreading, texture
formats or full application workloads are covered. EGL/Vulkan use the null
platform; advertised platform extensions do not prove X11/Wayland WSI works.
Readbacks are test assertions, not a proposed production presentation path.

## First device result: 29854870

Android 13 (SDK 33), model M2012K11AC. Vendor driver identifies itself as
Adreno 650, Vulkan 1.1.128, driver `0x801f6000`; GLES 3.2 V@0502.0,
GLSL ES 3.20, EGL 1.5. Use queried driver identity rather than inferred SoC.
Initial baseline source: `dcc3588`. Subsequent dispatch/tooling review starts
from `1599593`; exact binaries and dirty-source status are recorded per run.

| Case | Android native | glibc + hybris | glibc linked |
|---|---|---|---|
| Vulkan GPU fill + fence + readback | PASS | PASS | PASS |
| Dispatch (link/dlsym/GIPA/GDPA) | PASS | PASS | PASS |
| Life (2 devices, recreate, 2 threads) | PASS | PASS | — |
| Caps (refuse unadvertised, passthrough) | PASS | PASS | — |
| UBO 272B + injected wrong binding | PASS | PASS | — |
| Final library unload + process exit | PASS | CRASH | — |
| GLES context request 2, clear + shader draw + readback | PASS | PASS | — |
| GLES context request 3, clear + shader draw + readback | PASS | PASS | — |
| Native desktop GL context | unsupported | unsupported | — |

On this run `vkCmdBeginRenderingKHR` and `vkQueueSubmit2KHR` returned NULL;
`vkCmdBeginRendering` was present. That is a driver export observation, not a
Vulkan 1.3 claim. Both Vulkan paths report BC=false, ETC2=true, ASTC=true,
geometry/tessellation=true, shaderFloat64=false, shaderInt64=false.
These are capability queries, not tests of those features. Neither native nor
hybris advertises desktop OpenGL through EGL.

The corrected UBO fixture expects `255,255,0,255` for the correct binding
and `0,255,255,0` for the injected binding. Caps print `maxPush=128`,
`minUboAlign=64`, `dynamic_rendering=0`, `synchronization2=0`;
unadvertised `shaderFloat64` and a fake extension are refused (`-8` / `-7`).
The earlier custom-chain validation result is withdrawn as evidence of G04.

The 2026-09-06 rerun used `tools/build-aarch64.sh` from this checkout. Results
are under `build/results/<run-id>/` with `manifest.json` ELF hashes. Rebuild
the library when measuring a different source revision; a dirty working tree
is recorded as `source_dirty` in the manifest.

## Review checks

The manifest verification was checked against modified, missing, extra and
redirected SONAME ELF files. Run `20260906T224739-95b7e3db` completed with 17 PASS,
2 UNSUPPORTED (desktop GL), and 1 CRASH (hybris unload).
Both embedded widget shaders passed `spirv-val --target-env vulkan1.0`
and matched a fresh `glslangValidator -V --target-env vulkan1.0` compilation.
The unload crash is an open G03 gap: Vulkan operations finish successfully,
then the hybris process exits with SIGSEGV after the final `dlclose`.
The runner reports this as CRASH and returns nonzero; it is not an expected pass.
No new GPU feature or window-system compatibility is implied.
