# Initial smoke-test evidence

Historical coverage and device records from the initial September 2026 baseline. Statements about missing functionality below describe those revisions, not current HEAD. Use [the smoke-test entry point](README.md) for current usage and [the test index](../README.md) for later suites.

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
  generation-tagged object table. Workers start after the first instance
  exists, so they do not race first hybris entry.
- Unload: destroy Vulkan objects, `dlclose` the last frontend reference,
  then exit 0. Observed: `dlclose` returns 0 and the process exits 0.
  Not observed: the frontend or Android driver is unmapped, or that
  callbacks and objects are torn down. hybris keeps `libhybris-common`
  mapped using ELF `DF_1_NODELETE` because Android hook pointers and TLS
  cleanup callbacks can outlive `libvulkan.so.1`. The Android linker plugin
  stays held by common. That is the supported close sequence; it is not
  Android driver unload.
- Init: two threads' first `android_dlopen` of the vendor Vulkan library
  through `libhybris-common`, with no frontend constructor first.
  Observed: both threads get a non-NULL handle, `vkGetInstanceProcAddr`,
  and the same positive SDK version. Not observed: a particular handle
  identity, driver unload, or the number of initialization executions.
  This hybris-specific check is not scheduled for native. Failure to load
  common is a failure, not an unsupported result. If the second worker
  cannot be created, main releases the first from the barrier before joining.
- TLS: a worker creates and destroys Vulkan objects through the frontend,
  the main thread then `dlclose`s the last frontend reference, and the
  worker returns afterwards. Observed: `dlclose` returns 0, the worker
  joins, the process exits 0. Android TLS allocation and destructor execution
  are not instrumented; this workload alone does not prove either occurred.
  No library unmapping is checked. If Vulkan setup/teardown fails, the
  frontend is retained while the worker exits and the probe reports failure.
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
| Final frontend dlclose + process exit | PASS | PASS | — |
| Concurrent first android_dlopen | — | PASS | — |
| Worker TLS after frontend dlclose | PASS | PASS | — |
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

Run `20260906T233143-5f5b13ed` (library built from dirty `2a2b8b0`)
completed with 21 PASS and 2 UNSUPPORTED (desktop GL). The hybris-specific
`init` case is no longer scheduled for native. hybris `init` returned
handles, entry points and SDK 33 to both workers; native/hybris `tls`
closed the frontend and joined the worker. These are not observations of
TLS allocation/destructors or unmapping.

A temporary preload made the second `pthread_create` return EAGAIN on
the device: `init` released/joined the first worker and exited 2 without
timeout. A missing `PROBE_COMMON` also exited 2. No fault-injection code
is part of the probe or library.

## Review checks

The manifest verification was checked against modified, missing, extra and
redirected SONAME ELF files. A fresh library build verified the ELF
`NODELETE` flag with `readelf -d`. Run `20260906T233143-5f5b13ed` completed
with 21 PASS and 2 UNSUPPORTED. The earlier hybris unload SIGSEGV
(`20260906T224739-95b7e3db`) is closed for this probe by pinning
`libhybris-common`; vendor objects remain live. Both embedded widget
shaders passed `spirv-val --target-env vulkan1.0` and matched a fresh
`glslangValidator -V --target-env vulkan1.0` compilation.
No new GPU feature or window-system compatibility is implied.

The common library uses the link-time `-Wl,-z,nodelete` setting
([GNU ld documentation](https://sourceware.org/binutils/docs/ld/Options.html)),
so residency does not depend on reopening its pathname or an unchecked
`dlopen` result. It does not disable process-exit finalizers.

Supported close sequence: destroy Vulkan device/instance, drop the
frontend glibc reference, exit the process. `libhybris-common` and the
Android linker plugin stay mapped. First linker init uses `pthread_once`. The bundled linker initialization
paths do not reenter public `android_*` wrappers; new callbacks must preserve
that constraint. This probe does not exercise recursive initialization or
all possible schedules. It does not unload the Android driver.
