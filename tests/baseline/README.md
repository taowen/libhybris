# Headless GPU baseline

One probe executable, built from shared C sources for glibc and bionic,
run on the same Android device.
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

`tools/build-aarch64.sh` works in an independent checkout. The default
builder is owned by this repository: `tools/ensure-builder.sh` uses
`tools/container/Containerfile.aarch64`, a digest-pinned Debian base and
the 2026-08-01 package snapshot. Host prerequisites are x86_64 Linux,
Podman, Git, tar, Python 3.10+, sha256sum and file; running also needs adb.
The first build downloads the compiler and dependencies.

`tools/fetch-android-headers.sh` fetches Halium Android 11 headers at
`2c6ac3dcc4f8db593dd69906b0ec22822abfed91` and verifies cached content.
The default cache is `build/deps`; `HYBRIS_DEPS_DIR` may override it.
Use `--headers` for custom headers or `BUILDER_IMAGE` for a custom image.
Overrides record actual inputs but do not inherit the default recipe's
reproducibility claim.

Fresh source/header snapshots are taken before compilation. The library
manifest contains their file hashes, the container image ID, compiler
version, installed package versions, configure arguments and staged ELF
sha256/build-id. The recipe and build script are also fingerprinted.
The source commit/dirty flag is supplementary to these content identities.
Staging rejects stale plugins such as `vulkanplatform_x11.so`.

`tests/baseline/build.sh` compiles `probe-glibc`, `probe-glibc-linked` (DT_NEEDED libvulkan)
and `probe-bionic` from a source snapshot. Set `ANDROID_NDK_HOME` or an
explicit `BIONIC_CC`. It uses the same pinned glibc builder by default;
`GLIBC_CC` is an explicit override. `bundle/probe-manifest.json` records
source hashes, compiler identity and the three executable hashes.

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
staged files. Per-case `*-mappings.json` records observed file-backed mappings
at completion and, for unload/TLS cases, before closing the frontend.
Staged paths are associated with their hashes. Android paths are hashed
after execution in `android-mapped-files.sha256`; errors are retained.
These snapshots do not capture every historical mapping or verify live
mapped pages. `device.json` includes commands and queried driver strings.
Custom library paths do not inherit
the default manifest; supply their matching manifest explicitly. Results go under `build/results/<run-id>/`.
When a probe manifest is supplied in the bundle, changed executables are
rejected before deployment and staging is checked again.
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


## Source layout

probe.c dispatches modes and installs the watchdog. probe_common.c
contains symbol lookup, memory and queue selection helpers. EGL, Vulkan fill,
dispatch, lifecycle, capabilities and widget rendering live in separate
probe_*.c translation units. build.sh uses the same explicit source list
for bionic, glibc and the directly linked glibc variant.


## Standalone build verification

A temporary checkout outside the parent project used the repository's default
builder and downloaded headers, then built all library and probe artifacts.
The builder image ID was
`62d617eddf3720b5c3574f6666c59bcb2abb7deb03f74a6801820b20fc874704`;
the header snapshot hash was
`fd30a127eb85c3258cf471906cb002b1f1e110acdccb6247c5dc3aee3a1ddffc`.

- 29854870: `20260906T235359-6ecadb60`, 21 PASS / 2 UNSUPPORTED.
- KB2000: `20260906T235440-110adca9`, 21 PASS / 2 UNSUPPORTED.

Both unsupported cases are desktop GL. On 29854870 the TLS probe recorded
both before-close and completion mappings, including common and vendor
libraries. All 93 observed Android file paths were hashed without error.
Temporary modified-header and modified-executable checks were rejected.
These are standalone smoke results, not CTS or Blender regression results.

The final runner rerun on 29854870, `20260906T235759-0a9d4916`,
also completed with 21 PASS / 2 UNSUPPORTED, retaining all 23 commands,
nonempty mapping snapshots and driver/probe identities.


## Optional standard Vulkan loader

The build also produces libhybris-vulkan-icd.so.0. It calls the vendor HAL
directly through hybris, leaving dispatchable-object headers to the standard
glibc loader. This path is opt-in and currently headless; no system ICD JSON
is installed. See [adapter contract](../../hybris/vulkan/icd/README.md).

Supply a glibc AArch64 standard loader from the pinned builder (its package is
libvulkan1 1.4.309.0-1). For example, copy
/usr/lib/aarch64-linux-gnu/libvulkan.so.1 out of the image produced by
tools/ensure-builder.sh. Then run:

    python3 tests/baseline/run.py --serial 29854870 \
      --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
      --vulkan-loader /path/to/standard/libvulkan.so.1

The runner stages that loader separately from the hybris frontend, records
its hash and selects the ICD via VK_DRIVER_FILES. The loader path must refer
to the standard loader, not libhybris's replacement libvulkan. HAL selection
is explicit for this experiment. The eight extra cases cover fill/readback,
dispatch, lifecycle, unload/thread exit, caps, widget pixels and direct linking.

Verified runs: 29854870 20260907T001046-6fb3cd47 and KB2000
20260907T001144-6f996d48, each 29 PASS / 2 UNSUPPORTED (desktop GL).
The ICD mappings contain the standard loader and vendor HAL, without Android
libvulkan. These results do not verify WSI, Vulkan 1.1 workloads, validation,
capture/replay, or complete physical-device command coverage.


## Standard validation layer

Fetch the optional glibc AArch64 layer with:

    bash tools/fetch-validation-layer.sh

This downloads Debian vulkan-validationlayers 1.4.309.0-1 from the same fixed
snapshot as the loader and verifies the package SHA256. It uses dpkg-deb from
the host or the repository builder. It does not run automatically during
ordinary builds or baseline runs.

Add both options to the standard-loader command above:

    --validation-layer tests/baseline/build/validation/extracted/usr/lib/aarch64-linux-gnu/libVkLayer_khronos_validation.so
    --validation-manifest tests/baseline/build/validation/extracted/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json

The original layer manifest is retained except for its staged library path.
Both file hashes are recorded. The validation mode explicitly enables
VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils through the standard
loader; it does not construct a layer chain or edit dispatchable handles.

The legal instance/device/buffer lifecycle must emit zero ERROR messages.
A zero-size buffer must emit exactly VUID-VkBufferCreateInfo-size-00912;
the debug callback returns VK_TRUE during injection so validation stops that
invalid call before the vendor, with VK_ERROR_VALIDATION_FAILED_EXT. Other
errors fail the probe, as does a missing/unloadable layer.

Initial verified runs: 29854870 20260907T001810-cfa7fcb1 and KB2000
20260907T001900-e6551fc2, each 30 PASS / 2 UNSUPPORTED (desktop GL).
This verifies the standard validation entry path for a small workload.
It does not validate every baseline draw or close the capture/replay gate.

Run 20260907T002102-c49a311d also passed (30 PASS / 2 UNSUPPORTED).
The validation-active snapshot verifies the layer mapping before instance
destruction; the completion snapshot alone can miss the unloaded layer.
Both layer and original manifest hashes are recorded.


The same layer options now also run ubo-validation. This executes both widget
bindings with an explicitly enabled VK_EXT_validation_features synchronization
validation feature, a live debug-utils messenger, and the existing exact
pixel assertions. Both are legal API workloads: the alternate binding checks
a known different shader result, not an invalid Vulkan call.

Runs 20260907T002320-36535264 (29854870) and
20260907T002358-2143b043 (KB2000) each completed 31 PASS / 2 UNSUPPORTED.
Both widget renders produced their expected pixels with zero validation
ERRORs, including the upload/render/transfer/host-readback and destruction
paths. This is fixed-fixture coverage; arbitrary applications, WSI and
capture/replay are still not verified.


## Optional headless capture/replay

Build the pinned GFXReconstruct source and its AArch64 runtime dependencies:

    tools/build-capture-tools.sh

Add this option to the standard-loader command above:

    --capture-tools tests/baseline/build/gfxreconstruct/install

This adds `icd-capture-replay`. It runs `ubo-good` and `ubo-bad` separately
because the capture manager ends its recording when the last instance is
destroyed. For each binding it compares the uncaptured probe, captured probe
and replayed `vkCmdCopyImageToBuffer` output: all 1024 bytes of the 16×16 RGBA8
image must match exactly. The existing probe still verifies its expected
center pixel. `PROBE_WIDGET_DUMP_DIR` enables raw image output only when set.

The runner derives command indices from `gfxrecon-convert` output and verifies
that the resource report names the selected copy/submit. `capture/` stores
both `.gfxr` files, API JSONL, dump requests, resource reports, raw pixels,
command logs and `comparison.json`. Capture tool files are checked against
the build manifest before staging; `device.json` records that manifest and
staged tool/runtime hashes. The builder records the fixed source/submodule
revisions, image identity and package inventory hash. Optional tools are never
downloaded or activated by the ordinary baseline run.

Scope: two headless submissions, no swapchain or `vkQueuePresentKHR`.
GFXReconstruct consequently reports "File did not contain any frames" even
though it replays the draw and dumps the copy successfully. This is not a
presented-frame, cross-driver, application or WSI replay claim. Replay resource
dumping adds instrumentation; uninstrumented capture is compared separately.

Verified 2026-09-07: 29854870 `20260907T004458-8576f807` and KB2000
`20260907T004459-930906e9`, each **32 PASS / 2 UNSUPPORTED**, including VVL,
SyncVal and capture/replay. Both devices produced identical complete images:

- good RGBA SHA256: `34cfd029fad3bcac2285a3c2c669ac33dbf553c72814ae52a98020fa58359645`
- bad RGBA SHA256: `4f41dc70de144b8947613e185f53a602b28ae5454d27ea819a3c3a46ff254bca`

The first attempted run failed because the tools lacked the indirect
`libxxhash.so.0` dependency; the fixed builder includes it. That failure is
not counted as a successful capture. The ordinary probe baseline remained
31 PASS / 2 UNSUPPORTED during that failed tool run.


## Registry and execution routes

`tools/registry/generate-dispatch-coverage.py` checks the SHA256 of Vulkan-Headers
v1.4.309 registry (`952f776f6573aafbb62ea717d871cd1d6816c387`) and generates
[command metadata](../../tools/registry/dispatch-coverage.json) plus the probe's
`dispatch_commands.inc`. Regenerate with:

    python3 tools/registry/generate-dispatch-coverage.py /path/to/vk.xml

Only commands provided by the Vulkan API are included (726); Vulkan SC-only
commands and disabled extensions are excluded. The metadata preserves aliases,
first parameter, scope, first core version and core/extension providers. It is
a query-coverage catalog, not a capability declaration or a feature evaluator.
The snapshot built into each probe includes the generated table's hash.

Each dispatch case writes `*-registry.json` with dlsym, GIPA(NULL),
GIPA(instance), GDPA(device) and linked-address availability for every name.
The probe verifies the 137 required core 1.0 entries and rejects non-global
commands returned by GIPA(NULL) (with GIPA's version-dependent self-lookup
exception), and non-device commands returned by GDPA. Existing specific
unenabled-extension checks remain. This is not every resolver rule or all
extension-enable combinations; later pointers are recorded but not executed.
See the [GIPA contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html)
and [GDPA contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceProcAddr.html).

`vk`, `vk-dlsym` and `vk-gdpa` run the same 4096-byte fill/fence/host-readback
workload. The linked binary's `vk` case now uses linked function addresses for
all workload calls; GDPA uses GIPA for instance/physical-device operations.
Earlier linked-vk results only established library dependency loading, since
the workload still called GIPA-derived pointers. Linked dispatch had separately
called create/destroy through linked symbols and retains that coverage.

`vk-core11` requests Vulkan 1.1; `vk-khr11` requests Vulkan 1.0 and explicitly
enables VK_KHR_bind_memory2 and VK_KHR_get_memory_requirements2. Both use their
corresponding GDPA aliases to query requirements and bind memory, compare
requirements against the 1.0 query, and complete the same GPU readback. Missing
versions/extensions report UNSUPPORTED. These cases do not claim complete
1.1 support. Neither connected device advertises dynamic rendering or
synchronization2, so their core/KHR execution remains unverified.

Verified runs: 29854870 `20260907T005630-74134b03` and KB2000
`20260907T005631-3e16a681`, each **45 PASS / 2 UNSUPPORTED**, including validation
and capture/replay. All five dispatch variants check 726 names with zero scope
errors. Native/hybris/standard-ICD dlsym availability was 232/632/269 commands;
GDPA returned 138 commands in each configuration. These are pointer-resolution
counts, not supported feature counts or a compatibility percentage.


Export split regression: rebuilt libraries and probes, compared all 643 defined
dynamic frontend exports (name/type/binding/visibility), then ran 29854870
`20260907T010200-b47ca796` and KB2000 `20260907T010201-038aaa84`.
Both are **45 PASS / 2 UNSUPPORTED**. All five registry-query reports per device
match their pre-split run exactly. The current fixed AArch64/Wayland-enabled
build is verified; this is not a new platform/build-configuration matrix.


## Capability differences

`caps` writes named `CAP_VALUE` records; the runner preserves them in
`*-caps-values.json` and compares native with hybris and optional ICD in
`capability-differences.json`. The generator
`tools/registry/generate-capability-fields.py /path/to/vk.xml` verifies the same
pinned registry hash as the dispatch generator. It emits 174 named scalar
values including all 55 core features, 106 limit members (arrays expanded),
and five sparse properties, without reading padding or assuming struct packing.
Device identity/version, extension spec versions and ten selected formats are
also recorded. Image-format queries use 2D, optimal tiling, sampled plus
transfer-destination usage and zero flags; error results are retained, and
undefined output values after FORMAT_NOT_SUPPORTED are not serialized.

Runs `20260907T010645-554aab1d` (29854870) and `20260907T010646-7003fc92` (KB2000)
each completed **42 PASS / 2 UNSUPPORTED**, without optional VVL/capture cases.
Each native record has 325 values. Frontend differences are empty on both
devices. ICD exposes VK_ANDROID_native_buffer version 8 absent in native, and
lacks native VK_EXT_hdr_metadata, VK_GOOGLE_display_timing,
VK_KHR_incremental_present, VK_KHR_shared_presentable_image and VK_KHR_swapchain.
The adapter directly forwards HAL device discovery, while native uses Android's
loader; this is consistent with their different WSI ownership and is not an
implemented compatibility transformation. No image or presentation semantics
are proved by these queries. The report records differences without changing
advertised capabilities or treating all differences as failures.

Scope remains the first enumerated physical device, core 1.0 structures and
the ten explicit format queries. Features2 extension chains, all devices,
format creation/usage combinations and workaround reasons are not covered.
Existing unsupported-feature/unknown-extension rejection and positive
CreateDevice checks remain in the same `caps` workload.


## Features2 chain

`caps2` requests Vulkan 1.1 and queries a chain containing 16-bit storage,
multiview, variable pointers, sampler YCbCr conversion and shader draw
parameters (11 feature bits). It checks that the chain pointers survive the
query, compares all 55 core features by name with the legacy query, and passes
the returned chain to CreateDevice with pEnabledFeatures left NULL. A false
shaderFloat64 bit is then deliberately enabled through that same chain and
must produce VK_ERROR_FEATURE_NOT_PRESENT. Versions below 1.1 are UNSUPPORTED.
The generated `feature_compare.inc` avoids comparing struct padding.

Runs `20260907T011042-0affc534` (29854870) and `20260907T011043-11791534` (KB2000)
each completed **45 PASS / 2 UNSUPPORTED**, without optional validation/capture.
The native, hybris and ICD `*-caps2-values.json` files contain identical 66
feature values on each device; all six negative creates returned -8.
These observations are separate from the core capability difference report,
so the shorter features2 record cannot overwrite the format/limit snapshot.

This is five selected core 1.1 feature structures, not every features2
extension chain or KHR alias path, and no shader execution of these features.
Properties2 and extended format-query chains remain unverified.


## EGL context isolation and migration

`egl-life` creates two unshared GLES2 contexts and two RGBA8 pbuffers. It
checks that the second context initially cannot see the first context's
buffer, then retains distinct buffer sizes/bindings and red/green clear state.
Eight switch pairs on the main thread, eight on a worker and eight back on the
main thread verify current context/read/draw surfaces and exact RGBA pixels.
The main thread releases current before pthread_create; the worker unbinds and
calls eglReleaseThread before pthread_join returns. Resources are deleted and
contexts/surfaces destroyed, then the sequence repeats for three cycles.

Runs `20260907T011354-4882a8fc` (29854870) and `20260907T011355-938bfc04` (KB2000)
each completed **47 PASS / 2 UNSUPPORTED**, including native and hybris
`egl-life`; optional Vulkan validation/capture were not selected. This probe
requests GLES2 and checks buffer objects, clear state and pbuffer pixels. It
does not prove context sharing, simultaneous rendering, shader state migration,
actual Android TLS destructor execution, handle generation management, FD
leak freedom or full GLES2 conformance. Failure paths terminate the isolated
probe process rather than attempting recovery of a failed EGL context.


## Widget draw evidence

The optional capture case now requests the indexed draw's descriptors and raw
color attachment before/after the draw, in addition to the transfer output.
`draw_evidence.py` accepts only the fixed single-draw fixture. It checks actual
capture command ordering, updated set versus bound set, vertex/fragment UBO
buffer ID/offset/range, all 272 UBO bytes against the fixture input, the draw's
image ID versus the transfer source and all 1024 attachment bytes against the
uncaptured image. The before-image hashes must match between the two bindings;
after-image hashes must differ. Pipeline/layout/set/buffer/image IDs and
resource hashes are included in `comparison.json` with `first_divergent_draw`.

Runs `20260907T011825-d445bf4a` (29854870) and `20260907T011826-0c3dc4d9` (KB2000)
each completed **48 PASS / 2 UNSUPPORTED**, with capture enabled and VVL disabled.
Both identify draw block 60 in submit 66: set 24 uses buffer 5 for the correct
binding and buffer 6 for the injected binding; range is 272 and attachment is
image 11. The image is identical before draw and diverges after draw; the
attachment and final copy match exactly for each binding. These IDs are local
to each capture, not persistent runtime handles or generation IDs.

This extends fixed-fixture evidence only. It is not a general draw-state
tracker, arbitrary first-failure search, shader reflection validator, bounded
application capture or WSI/present lineage. Index buffer dumping is requested
from the tool but is not a verified output of this fixture. No new capture
format or replay engine is introduced. The preliminary checker run caught a
local begin/draw tuple-order bug; only the final runs above pass the completed
association checks. Older capture passes alone do not prove this new gate.

Bionic TLS source split regression: fresh library/probe builds preserve 130
common dynamic exports (name/type/binding/visibility) and PT_TLS file size 0,
memory size 1032 bytes, alignment 16. Runs `20260907T012238-92e0cbe2`
(29854870) and `20260907T012239-23559b33` (KB2000) each complete
**50 PASS / 2 UNSUPPORTED**, with VVL, SyncVal and draw capture evidence enabled.
No new claim of actual TLS destructor execution follows from this source split.

## Promoted TLS bounds

The hybris-only `tls-bounds` probe calls the existing linker callback in
isolated child processes, without loading Android/GPU libraries. Offset
addition wraparound, filesz larger than memsz, memsz beyond the static area
and a NULL source with nonzero filesz must each terminate with SIGABRT.
A zero-length segment at the exact end remains accepted. Core dumps are
disabled in these children; expected aborts are checked by the parent, not
classified as successful GPU calls or hidden as unsupported results.

Before the fix, run `20260907T012533-871fb7f5` failed the wraparound rejection:
the callback accepted SIZE_MAX + 2 because its addition wrapped. The fix checks
offset and remaining space by subtraction, and validates filesz/source before
allocation, copying or publishing to the registry. Cleanup key creation and
setspecific failures now abort with an explicit diagnostic instead of silently
losing ownership. Key exhaustion/setspecific failure injection remains untested.

Fresh library/probe builds preserve 130 common dynamic exports. Final runs
`20260907T012712-1e831443` (29854870) and `20260907T012714-d19f1840` (KB2000)
each complete **51 PASS / 2 UNSUPPORTED**, including VVL, SyncVal and draw
capture evidence. All four invalid-range children and the legal empty-end
case pass. This does not prove general ELF parsing safety, TLS destructor
execution or static TLS slot reclamation.

## Compiler-generated TLS destructors and first touch

The hybris-only `tls-dtor` case loads a bionic C++ DSO with a `thread_local`
object. Two builds use NDK's default emulated TLS and `-fno-emulated-tls` ELF
TLS respectively. The second ELF has PT_TLS (24 bytes initialized, 25 bytes
reserved) and AArch64 TLSDESC relocations. Both fixture binaries, source and
build command identity are included in the probe manifest and verified while
staging. The fixture intentionally imports __cxa_thread_atexit to exercise the
hybris hook and is not run as a native Android-loader case.

A glibc-created worker first touches the object, reads initial value 73 and
sets 1234. Main drops its Android DSO handle while the worker is paused; after
release and join, the callback must have run exactly once on the worker with
1234, and must not have run before thread exit. Three load/thread/close/join
cycles cover each variant. This observes actual C++ destructor execution,
not just a successful thread join or dlclose return.

Before the fix, both devices failed the ELF TLS variant with initial value 0:
`20260907T013303-3f85d4f7` and `20260907T013304-05719b7c`. The old static
TLSDESC resolver returned an offset without initializing a glibc thread that
had not called any bionic libc hook. Q linker's static resolver now calls the
TLS initializer before returning its offset, preserving GPR and vector
registers across that call. The callback is resolved during linker setup;
new helper symbols are hidden. This does not change the linker plugin callback
struct ABI.

Final fresh library/probe builds and runs `20260907T013843-a5cf04e4` (29854870)
and `20260907T013844-4eb30921` (KB2000) each complete **52 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and draw capture evidence. Both TLS models pass all
three cycles on both devices.

Limits: the static resolver now saves registers and invokes the existing
initializer (including its registry mutex) on each access. Performance and
signal-handler/reentrant access are not validated. IE TLS accesses that do
not call this resolver still require an initialized thread. This is not proof
of every vendor TLS destructor, compat heap cleanup, DSO unmapping or static
slot reclamation. Those remaining cases must be tested separately.

## TLS registration catch-up across threads

The isolated `tls-bounds` process also registers an initialized byte on main,
then a different byte on a fresh worker. The worker must see both initializers;
main must catch up with the second initializer while preserving a local mutation
of the first. No Android library or GPU object is loaded by this case, so its
synthetic offsets cannot overlap vendor TLS.

Before the fix, `20260907T014423-1a5cb51e` read worker values `0,29` instead
of `17,29`: registration advanced its per-thread cursor past modules registered
on other threads without replaying them. Registration now applies its missing
entries under the existing mutex before advancing that cursor. Previously
initialized entries are left intact.

Fresh library/probe builds and runs `20260907T014600-82060cd9` (29854870) and
`20260907T014600-448b50be` (KB2000) each complete **52 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Both workers read `17,29` and both
main threads preserve the mutated first value `41` while receiving `29`.
All 130 common dynamic exports remain unchanged. This directly verifies the
registry callback's replay bookkeeping, not concurrent ELF loading, static slot
reclamation or signal-safe initialization.

## Concurrent first Vulkan instance operations

`vk-init` loads the frontend, resolves its ELF exports, then releases four
workers from a condition-variable gate without making a Vulkan API call on
main. Two workers first create an instance and two first enumerate global
extensions. Each performs four create/physical-device-count/destroy cycles,
using both ELF and GIPA create routes. Failed thread creation cancels and joins
every started worker. Native, hybris and standard-loader ICD run this workload.

Code review found unsynchronized publication of the platform module before
its initializer returned and lazy writes to global create/enumerate pointers.
The frontend now serializes module initialization and proc setup with separate
pthread_once controls. Null and Wayland plugins resolve global pointers during
that setup. Missing global entries return initialization failure. The plugin
initializer must not reenter the frontend's platform API while its once is
running; the current initializers call gralloc/common setup only.

This workload does not deterministically reproduce the old data race and is
not a ThreadSanitizer result. The headless runs exercise the null platform;
Wayland changes are build-checked only. Surface maps, surface/swapchain function
caches, per-object dispatch and generation tracking remain separate work.

The platform-only change still failed this workload on 29854870
(`20260907T015316-00d0a8ae`, incomplete worker cycles) and KB2000
(`20260907T015316-458817a0`, watchdog timeout), while native and ICD passed.
Further review found that the pthread bridge could allocate multiple backing
mutexes for the same static Android mutex and overwrite its pointer. A thread
could then unlock a different mutex from the one it acquired. The bridge now
guards pointer lookup/publication with a short host mutex, released before
acquiring or waiting on the translated mutex. It rechecks before allocating.
Four-byte-aligned bionic storage requires memcpy; a discarded pointer-atomic
implementation triggered SIGBUS and is not part of the final change.

`mutex-init` calls real pthread imports in the bionic fixture DSO. Four threads
race to first use each of 32 static mutexes; an atomic occupancy counter checks
mutual exclusion, all workers join, and every mutex is destroyed. The fixture
asserts its mutex offset is four bytes to exercise Android's alignment. This
does not touch its TLS object. It uses the existing fixture build/manifest.

Fresh builds and runs `20260907T020119-b9cd6fa2` (29854870) and
`20260907T020119-e59eacf4` (KB2000) each complete **56 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. All Vulkan workers complete four
cycles on native/hybris/ICD; all mutex workers report zero overlap/errors.
Vulkan's 643 and common's 130 dynamic exports remain unchanged.

A separate 29854870 comparison reuses the first run's staged probe/fixture and
dependencies, replacing only the common library: archived pre-fix common
SHA256 `f9a90d389d254959e663e93d0fa81eb817b3d084fbcc0466d0c0a124c087df43`
times out (exit 142); final common
`a77a96a95056c483deb8b934a5d61bb5b0dd3a12a74d10afcfd41927f017e42c`
passes eight fresh-process repetitions. This is a mixed-artifact callback
regression comparison, not a full old-release baseline.

Remaining limits: condition-variable/rwlock lazy initialization still needs
review, process-shared behavior is unchanged, and lookup-lock performance,
fork/signal reentrancy, timed/recursive/errorcheck mutex semantics and allocation
failure injection are not validated by this normal-mutex workload.

## Static rwlock first use and reader sharing

`rwlock-init` reuses the lock workload in `probe_lock_init.c` with real rwlock
imports from the bionic fixture. Four workers race to write-lock each of 32
fresh static rwlocks; atomic occupancy must remain one inside every critical
section. Then all four workers read-lock each rwlock simultaneously and meet
at a barrier before any releases it. While all four readers hold the lock,
trywrlock must return EBUSY. Every worker joins and every rwlock is destroyed.
Thread creation failure releases and joins all started workers.

Before the fix, run `20260907T020611-1e79682d` on 29854870 times out in
`hybris-rwlock-init` (watchdog exit 142). Multiple threads could allocate and
publish different backing locks for one Android static initializer. The bridge
now rechecks and publishes under the existing host synchronization guard and
uses that guard when reading the pointer for unlock. It releases the guard
before acquiring or waiting on the user's rwlock. Allocation/init failures
produce explicit fatal diagnostics; those failure paths are not injected.

This covers process-private static write first use and subsequent read sharing.
It does not establish writer fairness, timed/try-read behavior, process-shared
rwlocks, fork/signal reentrancy, allocation cleanup on failure or lookup-lock
performance. Condition-variable lazy initialization remains separate work.

Fresh library/probe builds and runs `20260907T020805-a5abffff` (29854870) and
`20260907T020805-1a903c3d` (KB2000) each complete **57 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. All four rwlock workers report zero
errors on both devices; the prior mutex and concurrent Vulkan workloads also
pass. The common library retains the same 130 dynamic exports.

## Static condition-variable initialization and wakeup

`cond-init` uses 32 fresh, zero-initialized bionic condition variables with
four-byte alignment. A timed waiter and two threads calling signal/broadcast
start together. Main changes the predicate under the same bionic mutex that
the waiter uses, then broadcasts; the early signal/broadcast calls may cause
spurious wakeups. The waiter loops on the predicate with a 500 ms absolute
realtime deadline. Every wait/pulse must succeed, all threads join, and all
condition variables and mutexes are destroyed. No condition is destroyed while
a waiter is active. Failed worker creation cancels and joins started workers.

The old common library passed `20260907T021201-1f959389` on 29854870. This is
not a deterministic reproducer of lost wakeups. Code review found that signal,
broadcast and wait wrappers could concurrently allocate different backing
condition variables and overwrite one another's pointers. Lookup and first
allocation now share the existing host publication guard. Actual wait/signal
operations run after it is released. Initialization failures now have explicit
fatal diagnostics; allocation-failure paths are not injected.

This probe exercises timedwait's normal wakeup path, signal and broadcast. It
does not validate plain wait, explicit/monotonic clocks, relative deadlines,
expected timeout behavior, process-shared conditions, waiter cancellation,
destruction with waiters, allocation reclamation or guard performance. The
existing destroy hook's modification of glibc waiter metadata is unchanged and
must not be inferred safe from this legal teardown workload.

Fresh builds and runs `20260907T021359-9aa5efda` (29854870) and
`20260907T021359-51c1edc4` (KB2000) each complete **58 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. All three condition workers report
zero errors on both devices. Common's 130 dynamic exports remain unchanged.

## Synchronization allocation ownership split

`common/bionic_sync.c` owns the host publication guard, static mutex/condition/
rwlock allocation and pointer lookup. `bionic_sync.h` holds initializer values
and four hidden helper declarations. `hooks.c` keeps the API wrappers, explicit
initialization/destruction and shared-memory handle translation. Its size falls
from 3588 to 3480 lines; the new implementation file is 131 lines.

The extracted helper bodies match the previous source apart from internal
linkage. Rwlock pointer publication moves behind one private helper; shared
handle translation still occurs afterward. No allocation policy, locking
scope, condition clock or destruction behavior changes in this split. The
library's 130 dynamic exports match by name, type, binding and visibility;
none of the new cross-file helpers is exported.

Fresh library/probe builds and runs `20260907T021854-5ecc07d9` (29854870) and
`20260907T021854-fb23cbb2` (KB2000) each complete **58 PASS / 2 UNSUPPORTED**,
including the three synchronization first-use probes, VVL, SyncVal and
capture/replay. This structural change adds no compatibility coverage beyond
those existing workloads; their documented limitations still apply.

## Missing shared-memory backing

The hybris-only `shared-unavailable` case requires glibc's `/dev/shm` directory
to be absent; it reports unsupported if that precondition is not met. It
checks that the existing shared allocator returns zero and translation of an
AArch64 tagged offset returns NULL. It then calls actual bionic pthread
mutex/condition/rwlock attribute and initialization APIs through the fixture:
PROCESS_SHARED initialization must return ENOMEM for all three object kinds.
It does not destroy failed objects or exercise them as initialized locks.

Before the fix, `20260907T022257-f825a071` on 29854870 crashes with SIGSEGV
(exit 139) on the allocator call. Allocation and translation now check that
the backing store was opened before accessing its header. The three pthread
initializers propagate missing allocation/translation as ENOMEM instead of
passing NULL to glibc. This also guards a NULL private allocation, but malloc
failure is not injected by this workload.

This is verified failure handling, not process-shared lock support. It adds
no ashmem/memfd backend and does not validate successful shared mappings,
concurrent initialization, growth/remap, interprocess exclusion, shared-object
destruction or allocation reclamation. Existing shared rwlock destruction and
condition-wait semantics remain separate defects to address.

Fresh library/probe builds and runs `20260907T022445-38be09a4` (29854870) and
`20260907T022445-d0261787` (KB2000) each complete **59 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Each device returns ENOMEM (12) for
all three shared initializers. Common's 130 dynamic exports remain unchanged.

## WSI wrapper resolution scope

The swapchain wrapper now uses the current device's GDPA to obtain its backend
function before doing WSI preparation. Wayland create/destroy obtains backend
functions from the supplied instance's GIPA on each call; no first-instance
function pointer is retained globally. Missing creation functions are rejected
before allocating Wayland objects. A missing destroy function for a mapped
surface produces an explicit fatal diagnostic before removing the mapping.

`wsi-disabled` is a hybris-only negative case built on the capability workload.
It creates a device with no extensions, requires GDPA(CreateSwapchainKHR) to be
NULL, then deliberately calls the direct export with a zeroed swapchain create
description. The frontend must reject this with EXTENSION_NOT_PRESENT before
using WSI. This is an intentional invalid application call testing a hybris
guard policy, not a legal Vulkan workload or a normative Vulkan error test.
It is not run against native/ICD or validation paths.

Before the fix, `20260907T022910-9f4bfd8e` on 29854870 returned success (0)
from the direct export despite GDPA returning NULL. No usable swapchain is
established by that result. The new path returns -7 instead of using a global
ELF entry that can bypass device enablement.

Wayland changes are build-checked only: no compositor/surface lifecycle is
exercised by this headless negative case. Full per-instance/device state,
surface-capability wrapper resolution, mapping concurrency, destruction order,
generation tracking and successful swapchain creation/presentation remain open.

Fresh library/probe builds and runs `20260907T023048-a47b0dd0` (29854870) and
`20260907T023048-bac54a9c` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. The negative case returns -7 on both
devices. Vulkan's 643 dynamic exports remain unchanged.

## Core 1.1 properties2 chains

`caps2` also queries ID, subgroup, point-clipping, multiview, protected-memory
and maintenance3 properties together, then each separately. It compares named
fields, UUID bytes and valid LUID fields between the two forms. The core
properties returned by properties2 must match the legacy query: five scalar
identity fields, device name, pipeline cache UUID and 119 limits/sparse fields.
The latter comparisons are generated from the existing pinned vk.xml rather
than comparing structure padding. Invalid LUID/node-mask contents are ignored.

The runner writes `capability2-differences.json` independently of the original
capability comparison. On these devices caps2 saves 198 values: the previous
66 feature values plus 119 core limit/sparse values and 13 property-chain
values. The legacy identity comparison is an internal assertion, not an extra
set of CAP_VALUE records.

Fresh probe builds and runs `20260907T023635-4bd7451e` (29854870) and
`20260907T023635-1e550e61` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native, hybris and ICD pass the
expanded caps2 workload, and all 198 recorded values agree within each device.
Production code is unchanged in this batch. These observations do not prove
subgroup operations, multiview rendering, protected allocations, maximum-sized
resource creation, all extension property chains or full Vulkan 1.1 semantics.

## GLES share-group lifetime

`egl-life` now follows each isolated-context cycle with two GLES2 contexts in
one share group. The first creates a 16-byte buffer and a one-pixel RGBA texture
containing red. The second sees both objects, starts with array-buffer binding
zero, reads red through its own FBO, resizes the buffer to 32 bytes and uploads
green. The first sees the new size and green pixel, then is destroyed. The
second must still see the 32-byte buffer and exact green pixel before deleting
the shared objects and its context. The sequence repeats three times.

Each handoff uses glFinish and explicit resource rebinding/attachment. This
checks sequential shared-object visibility and survival of creator-context
destruction; it does not test simultaneous rendering, cross-context fences,
shared-object deletion while another context references it, buffer contents,
shader/program sharing or general object generation tracking.

Fresh probe builds and runs `20260907T023959-ac41857d` (29854870) and
`20260907T023959-961f4ab9` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native and hybris each pass all three
shared-context cycles on both devices. Production code is unchanged.

## Simultaneous current contexts on separate threads

The isolated half of `egl-life` now includes two workers, each owning one
context and pbuffer. Both attempt to make their context current before main
opens the start gate. Each then performs eight state checks and clear/readback
iterations, requiring the original buffer binding/size and exact red or green
pixel. No mutex serializes their GL calls. Each releases thread state before
join, and main switches between both contexts to check preserved state again.
The sequence repeats for all three lifecycle cycles. Partial thread creation
releases the cancellation gate and joins all started workers.

This proves simultaneous current contexts and successful independent threaded
clear/readback. It does not establish GPU execution overlap, shader-draw
concurrency, shared-resource synchronization, application callbacks or general
generation tracking. Shared-context work remains sequential with glFinish.

Fresh probe builds and runs `20260907T024324-39d298a2` (29854870) and
`20260907T024324-8d0ade38` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native and hybris each report six
successful worker results across three cycles on each device. Production code
is unchanged in this batch.

## Surface capability fallback policy

The surface-capability wrappers now resolve Android-loader ELF trampolines
during frontend construction, eliminating lazy pointer writes from concurrent
calls. Missing entries return EXTENSION_NOT_PRESENT. Capabilities2 no longer
falls back to the legacy query, which cannot process the caller's input/output
pNext chains, or tries GIPA(NULL) for a physical-device command.

The 643 Vulkan dynamic exports remain unchanged. This is a code-review fix
with build and headless regression coverage, not a reproduced surface query
failure: no live WSI surface or missing-entry injection is exercised. Resolution
still uses Android-loader ELF trampolines; physical-device-to-instance state,
enabled-extension checks for these wrappers and live query-chain semantics
remain unfinished.

Fresh library/probe builds and runs `20260907T024657-4bc8a0d1` (29854870) and
`20260907T024657-607726a4` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. These results preserve the existing
headless baseline and do not close the live surface-query coverage gap.
