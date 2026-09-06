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

## Shader draws during context migration and concurrent use

The isolated contexts in `egl-life` now each retain a linked program and vertex
buffer for a full-screen triangle. Context 0 writes blue and context 1 yellow,
distinct from their red/green clear colors. Every existing iteration checks
GL_CURRENT_PROGRAM, draws without rebinding the program or vertex attributes,
and requires an exact center pixel and no GL error. This runs on main, through
single-worker migration, on two independently current worker contexts, then
again on main. Programs and buffers are deleted before context teardown.

This adds concurrent host-thread shader submission and program/vertex state
isolation to the previous clear/readback evidence. Programs are compiled on
main before migration; it does not test concurrent compilation, shared shader
objects, synchronization between shared resources, general GLSL compatibility
or overlapping GPU execution.

Fresh probe builds and runs `20260907T024950-483553de` (29854870) and
`20260907T024950-5322ce95` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native and hybris both pass the
expanded EGL lifecycle workload on each device. Production code is unchanged.

## Monotonic condition-variable aliases

The hybris-only `cond-clock` workload calls actual bionic imports of
pthread_cond_timedwait_monotonic and pthread_cond_timedwait_monotonic_np from
the fixture DSO. Each uses a fresh default-clock condition variable and a
CLOCK_MONOTONIC deadline 100 ms ahead, retaining that deadline across spurious
wakeups. It requires ETIMEDOUT and at least 90 ms elapsed, then unlocks and
destroys the objects. The existing process watchdog bounds excessive delays;
there is no tight upper timing assertion sensitive to host scheduling.

Before the fix, `20260907T025314-ab535531` on 29854870 returns ETIMEDOUT in
64,583 ns and 38,177 ns respectively: the aliases went to ordinary timedwait,
which treated monotonic timestamps as expired realtime deadlines. Both now use
the clockwait bridge with CLOCK_MONOTONIC explicitly. Fixture declarations
retain these legacy imports even where the current NDK hides their prototypes.

This verifies process-private absolute timeout behavior for the two aliases.
It does not validate wall-clock changes, relative waits, shared conditions,
cancellation, wakeup fairness or all condition-variable attribute combinations.

Fresh library/probe builds and runs `20260907T025449-9f0dad86` (29854870) and
`20260907T025449-72b2c6b8` (KB2000) each complete **61 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Both aliases return ETIMEDOUT after
about 100–102 ms on both devices. Common's 130 dynamic exports are unchanged.

## Relative condition wait validation

`cond-clock` also exercises pthread_cond_timedwait_relative_np through the
bionic fixture: 100 ms must end with ETIMEDOUT after at least 90 ms, while
nanoseconds equal to one billion, negative nanoseconds, negative seconds and
an unrepresentable LONG_MAX-second deadline must return EINVAL. Spurious
wakeups retry the relative duration; the no-signal workload does not assert a
tight upper timing bound. Conditions are explicitly initialized so invalid
inputs can be followed by legal destruction without requiring lazy allocation.

Before the fix, `20260907T025708-f2f78e33` on 29854870 incorrectly returns
ETIMEDOUT for one-billion nanoseconds after about one second, negative
nanoseconds after about 1.3 ms, and LONG_MAX seconds immediately. The wrapper
now validates the duration, uses checked addition for the absolute deadline,
and calls the common clockwait bridge with CLOCK_MONOTONIC. This removes the
duplicated translation path and dependence on a realtime absolute deadline.

No wall-clock jump is injected, and shared conditions, cancellation, wakeup
fairness and boundary arithmetic on 32-bit platforms remain unverified.

Fresh library/probe builds and runs `20260907T025846-dffa5ff2` (29854870) and
`20260907T025846-d4473ad5` (KB2000) each complete **61 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Relative 100 ms waits complete in
about 100–102 ms; all four invalid-duration cases return EINVAL (22). The 130
common dynamic exports remain unchanged.


Mutex/condition hook split regression: `bionic_sync.c` now owns the 17 API
hooks and Android condition pulse helpers in addition to static publication.
`hooks.c` keeps the unchanged registration table. Compared the moved bodies
and registration against the preceding revision; rebuilt libraries and probes.
All 130 common and 643 Vulkan defined dynamic symbols remain unchanged, and
the 17 cross-file hook entry points remain private.
Runs `20260907T030507-d17a9d9b` (29854870) and
`20260907T030507-63e6a157` (KB2000) each completed **61 PASS / 2 UNSUPPORTED**,
including mutex/rwlock/condition first use, condition clock/error handling,
shared-allocation failure, EGL/Vulkan, validation/SyncVal and capture/replay.
This structural change does not establish working Android process-shared
synchronization, destruction with waiters, 32-bit ABI coverage or fairness.


`sync-destroy` checks five unused private static initializers: normal,
recursive and errorcheck mutexes, a condition and a rwlock. Each is destroyed,
explicitly reinitialized in the same storage, used and destroyed again.
`sync_fixture.h` supplies identical bionic lifecycle code to the native probe
and the Android DSO loaded by hybris. It is included in the probe manifest.
The old common library returned EINVAL for the first normal mutex in
`20260907T030830-362e5220`; that run's native fixture load did not reach the
workload and is not native semantic evidence. The final native probe compiles
this lifecycle directly, avoiding unrelated TLS fixture imports.
After rebuilding, runs `20260907T031048-dcf57325` (29854870) and
`20260907T031048-d72e6be0` (KB2000) each completed **63 PASS / 2 UNSUPPORTED**,
including native/hybris success for all five kinds, validation/SyncVal and
capture/replay. The 130 common and 643 Vulkan defined exports are unchanged.
No claim is made about double destroy, destruction while in use, 32-bit ABI,
working shared synchronization or reclamation of the shared allocator.


Rwlock hook split regression: moved 15 rwlock/attribute hooks and their handle
lookup from `hooks.c` into `bionic_sync.c`. The two existing public kind
accessors remain exported; other moved entry points are hidden. Bodies and
registration were compared to the preceding revision; all 130 common and
643 Vulkan defined exports remain unchanged after rebuilding.
Runs `20260907T031532-6df22c8a` (29854870) and
`20260907T031532-c897f4f0` (KB2000) each completed **63 PASS / 2 UNSUPPORTED**,
including rwlock first use, static destruction/reinitialization, TLS, graphics,
validation/SyncVal and capture/replay. This is structural regression evidence;
shared rwlock destroy translation, timed-lock semantics, kind preferences and
fairness are not newly verified by the existing first-use workload.


`rwlock-kind` checks bionic rwlock attribute default, both supported kind
round trips, lock creation/read/write/destruction for each kind, and EINVAL
for -1, 2, 3 and INT_MAX without changing the last valid kind. It shares the
bionic lifecycle source between the native probe and the DSO used by hybris.
Bionic's nonrecursive-writer enum is 1, while glibc's matching enum is 2;
passing values through selected a different host policy and accepted the
bionic-invalid 2. See the [AOSP attribute implementation](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r3/libc/bionic/pthread_rwlock.cpp).
Before the fix, `20260907T031839-af08782d` passed natively and failed through
hybris because setting 2 returned success instead of EINVAL. The bridge now
maps both directions explicitly and rejects the host-only policy domain.
The workload does not establish starvation freedom, scheduling order,
recursive-reader misuse behavior, timed locks or cross-process synchronization.
After rebuilding libraries and probes, `20260907T032015-6cdbdcb6` (29854870)
and `20260907T032015-9862e194` (KB2000) each completed **65 PASS / 2 UNSUPPORTED**,
including native/hybris kind cases, validation/SyncVal and capture/replay.
The 130 common and 643 Vulkan defined dynamic export sets remain unchanged.


Shared-handle correction: mutex timedlock/timeout_np now translate a hybris
shared-memory tag before passing it to glibc; shared rwlock destruction now
translates before destroy rather than afterward. Missing translation returns
EINVAL in these three paths. No shared-memory allocator, Android-native shared
mutex no-op policy, or condition wait policy is changed.
Current devices lack /dev/shm, and no ready proot binary was available in the
workspace. The existing `shared-unavailable` case covers allocation/translation
failure and failed initialization, not these successful shared-object paths.
This change therefore has source review, build and existing private-path
regression evidence; working process-shared timed waits/destruction remain
unverified, as do allocator growth/remapping and cross-process coordination.
Rebuilt libraries/probes and completed runs `20260907T032439-4c9cf97d`
(29854870) and `20260907T032439-0e18b5ae` (KB2000), each **65 PASS /
2 UNSUPPORTED**, including validation/SyncVal and capture/replay. The 130
common and 643 Vulkan defined exports remain unchanged. These counts do not
close the shared-path coverage gap described above.


ICD instance ownership regression: `icd-vk-init` enables the bounded
`HYBRIS_ICD_INSTANCE_TRACE=1` diagnostic and validates 16 unique generations,
matching create/destroy handles and no remaining records. Its evidence parser
hash is recorded in device.json; `icd-vk-init-instances.json` records measured
peak concurrency and reused raw handles. Other workloads keep tracing off.
Rebuilt runs `20260907T033203-27daa0de` (29854870) and
`20260907T033203-b19723b3` (KB2000) each completed **65 PASS / 2 UNSUPPORTED**,
including validation/SyncVal and capture/replay. Both record 16 creations and
16 destructions with zero records remaining. Check each evidence JSON for
observed concurrency/reuse; absence of reuse does not prove reuse handling.
The common 130, Vulkan 643 and ICD 3 defined export sets remain unchanged.
This covers ICD instance records only, not frontend/device/resource state,
custom allocation callback failure/locking, trace truncation or malformed
HAL behavior. Destroy records precede backend destruction and do not imply
GPU completion or driver unloading.


The concurrent instance probe now uses two barriers per round: all four
instances remain alive before the first destroy, and all destroys finish
before any next-round create. All workers complete the barriers even after
create/query/global-enumeration errors; partial thread startup cancels before
entering them. `instance_evidence.py` now requires peak_live=4, in addition to
16 unique, paired lifetimes. The previous peak-2 and peak-1 trace files were
rejected by this stronger gate. This verifies overlapping object lifetimes,
not parallel execution inside the HAL, handle reuse or error-injection paths.
Rebuilt runs `20260907T033524-371d9b4f` (29854870) and
`20260907T033524-e279f2dc` (KB2000) each completed **65 PASS / 2 UNSUPPORTED**,
including native/hybris/ICD concurrent instance workloads and validation/capture.
Both evidence files report created=16, destroyed=16, remaining=0, peak_live=4,
reused_handles=0. Thus actual raw-handle reuse remains unverified.


`vk-alloc` supplies application allocation/reallocation/free callbacks for
three instance create/query/destroy cycles and checks that outstanding callback
allocations return to zero after each destruction. A final create refuses
all callback allocations and requires VK_ERROR_OUT_OF_HOST_MEMORY without
outstanding allocations. It runs through native, replacement frontend and
standard ICD. The callback allocator preserves alignment and realloc contents;
this does not test every allocation failure position, callback locking,
device/resource callbacks or concurrent allocation races.
Rebuilt runs `20260907T033851-afd02fc4` (29854870) and
`20260907T033851-da205be7` (KB2000) each completed **68 PASS / 2 UNSUPPORTED**,
including all three allocator paths, validation/SyncVal and capture/replay.
All successful cycles ended with live=0, and refusal returned -1
(VK_ERROR_OUT_OF_HOST_MEMORY). The standard loader may reject allocation
before reaching the ICD; this is API-boundary failure evidence, not proof of
failure at the ICD record allocation or every HAL allocation site.


`icd-alloc-direct` explicitly loads the optional adapter and calls its
`vk_icdGetInstanceProcAddr` entry, without loading the standard Vulkan loader.
Its first application allocator call refuses the adapter's instance record;
the probe requires OUT_OF_HOST_MEMORY, exactly one allocation attempt and
zero outstanding allocations. It then restores allocation and completes three
instance create/query/destroy cycles, followed by another allocation refusal.
This is a direct ICD/HAL boundary workload, not standard-loader or layer-chain
coverage. Existing `vk-alloc` cases retain those separate paths. No dispatch
header rewriting is needed for these direct instance queries/destruction.
Rebuilt runs `20260907T034217-545f66e6` (29854870) and
`20260907T034217-f1c385c1` (KB2000) each completed **69 PASS / 2 UNSUPPORTED**,
including validation/SyncVal and capture/replay. Both direct cases record
initial-reject=-1, calls=1, live=0; each recovery cycle also ends at live=0.
Their final mapping records contain neither the standard nor Android Vulkan
loader. This proves the tested adapter allocation refusal and subsequent
recovery, not all allocation failure positions or callback locking interactions.


The direct allocator workload also allows the adapter record allocation and
then refuses all subsequent allocations, exercising HAL creation failure and
adapter-record rollback. It requires at least two allocation attempts,
OUT_OF_HOST_MEMORY and no outstanding callback allocations, then restores
allocation for three normal cycles. This targets the first required HAL
allocation on the tested driver, not every optional allocation or failure site.
Allocation callbacks must not call Vulkan commands; earlier references to
callback reentrancy were not a valid API-conformance requirement. See
[Vulkan host allocation rules](https://docs.vulkan.org/spec/latest/chapters/memory.html).
Callbacks still run outside the instance table guard to avoid holding it
across application allocator locks and work.
Rebuilt runs `20260907T034553-3c4de9ce` (29854870) and
`20260907T034553-5b6f5c0c` (KB2000) each completed **69 PASS / 2 UNSUPPORTED**,
including validation/SyncVal and capture/replay. Both direct cases report
HAL-reject=-1, calls=2, live=0, followed by three successful recovery cycles.


Stdio module split: `bionic_stdio.c` owns 78 compiled FILE hooks, standard
stream mapping/storage, wide streams, mount-table streams and buffer queries.
The central registration stays in `hooks.c`, which drops 695 lines. Moved
function bodies and hook registrations were compared with the prior revision;
the existing public endmntent hook remains public and new cross-file symbols
remain hidden. Defined export sets remain common=130, Vulkan=643, ICD=3.
This is structural regression coverage using the existing workloads, not
exhaustive stdio semantics: 32-bit FILE/offset ABI, all wide/mount-table
operations, error states and concurrent stream access remain unverified.
Rebuilt runs `20260907T034933-8421f3d3` (29854870) and
`20260907T034933-d7d8d7cd` (KB2000) each completed **69 PASS / 2 UNSUPPORTED**,
including allocation callbacks, synchronization, TLS, validation/SyncVal and
capture/replay. No new API capability is claimed by the file split.


`stdio` runs the same bionic fixture natively and through Android DSO imports.
It writes buffered file content, calls fflush(NULL), and checks bytes via
pread before closing; it also flushes an open_memstream and checks its
published pointer, length and contents before closing. Its file lives in the
runner's isolated directory and is removed on success. The shared source is
part of the probe manifest. The old common library crashed with exit 139 in
`20260907T035222-4e6ec2cc`, while native passed. The memory case was not reached
in that old hybris process and has no separate captured pre-fix failure.
The bridge removes the descriptor precheck from fflush and fflush_unlocked:
NULL must reach glibc's flush-all operation, and descriptorless memory streams
must still flush. The unlocked entry is not independently exercised here;
concurrent flushing, input streams and wide-stream cases remain unverified.
Rebuilt runs `20260907T035349-1a7cf3cc` (29854870) and
`20260907T035349-ede7da43` (KB2000) each completed **71 PASS / 2 UNSUPPORTED**,
including both stdio cases in native and hybris, validation/SyncVal and
capture/replay. Defined exports remain common=130, Vulkan=643, ICD=3.


The stdio workload also covers normal/64-bit fgetpos/fsetpos: save offset 3,
read one byte, restore and reread the same byte. Pipe queries require -1,
ESPIPE and bionic's position=-1 result. In old run `20260907T035705-cab681fa`,
native passed both sizes while hybris wrote position=549201271456 on the
ordinary pipe error; its 64-bit case was not reached. The bridge now uses
ftello/fseeko and their 64-bit variants, matching bionic's offset-only model
and avoiding reads of uninitialized/private glibc fpos fields. See the
[bionic implementation](https://fuchsia.googlesource.com/third_party/android.googlesource.com/platform/bionic/+/01e7576d8ba146a73fe9b1c3eb67130c471e06f0/libc/stdio/stdio.cpp).
These are AArch64 byte-stream cases; 32-bit overflow, multibyte conversion
state and large-file boundary cases remain unverified.
Rebuilt runs `20260907T035849-5ba9e5ab` (29854870) and
`20260907T035849-1dcc8353` (KB2000) each completed **71 PASS / 2 UNSUPPORTED**,
including ordinary/64-bit position cases, validation/SyncVal and capture/replay.
Both hybris pipe queries now return -1, ESPIPE (29), position=-1, matching
native. Defined exports remain common=130, Vulkan=643, ICD=3.


`ubo-dynamic` retains the 272-byte widget block and uses a dynamic uniform
buffer descriptor. It aligns four slots to minUniformBufferOffsetAlignment,
sets descriptor base=stride and binds dynamic offset=2*stride for good data
at slot 3. Slots 0/1/2 contain the alternate field values; ignoring either
base or dynamic offset therefore changes the expected pixel. A second legal
binding uses dynamic offset=stride and must produce the alternate color.
Native/frontend/ICD run both bindings. `ubo-dynamic-validation` adds VVL and
SyncVal on the ICD path. Base, dynamic offset, range and total size are logged.
This does not enlarge the shader block to 1.2KB, cover multiple dynamic
bindings, descriptor templates or re-record/resubmit, or add dynamic capture
resource reconstruction. Existing static-widget capture cases are retained.

Fresh library/probe builds and runs `20260907T040239-01b5cc15` (29854870)
and `20260907T040239-ec81c1a9` (KB2000) each report 75 PASS, 2 UNSUPPORTED.
Both devices use alignment=64, base=320, dynamic=640 (good) / 320
(alternate), range=272, allocation buffer size=1232. All three routes produce
exact good/alternate pixels, and both ICD dynamic validation variants report
zero errors. Static validation/SyncVal and capture/replay remain passing.
The 1232-byte buffer contains four aligned slots; it is not a 1232-byte shader
block.


`ubo-large` extends the widget shader block to 1232 bytes: the original
272-byte prefix, fourteen column-major mat4 values at 272 (array stride=64,
column stride=16), three vec4 tail values at 1168, signed int at 1216,
32-bit bool storage at 1220 and vec2 end marker at 1224. Host static assertions
check offsets/size; the fragment shader checks every matrix element, tail
vector and end marker, returning magenta on mismatch. Valid blocks encode
bool/int/srgb into the expected good/alternate pixels. The matrices contain
distinct non-symmetric values, so a transpose or incorrect stride fails.
Both ordinary and dynamic descriptors run with both data variants;
`ubo-large-validation` repeats all four draws through ICD VVL/SyncVal.
This is a synthetic layout; it does not reproduce Blender's complete instanced
widget block, descriptor templates, staging copy or command re-record/resubmit.

The large embedded shaders are compiled from `widget.vert` / `widget.frag`
with `glslangValidator -V --target-env vulkan1.0 -DLARGE_UBO=1`, then validated
with `spirv-val --target-env vulkan1.0`. Their SPIR-V decorations confirm the
host offsets above, MatrixStride=16 and matrix ArrayStride=64. The original
shaders compiled without the define remain byte-for-byte identical to their
committed embedded arrays. The build snapshots both source and large embedded
arrays in the probe manifest.

Rebuilt runs `20260907T041010-737e6c4d` (29854870) and
`20260907T041010-39a5c131` (KB2000) each report 79 PASS, 2 UNSUPPORTED.
All four large-UBO variants produce the exact expected good/alternate pixels
on native/frontend/ICD; ICD VVL/SyncVal reports zero errors for each draw.
Dynamic descriptors use base=1280 and offset=2560 / 1280 with range=1232
and total buffer size=5072. Existing static capture/replay also passes.


`ubo-staged` uploads 272-byte and 1232-byte blocks with vkCmdCopyBuffer from
host-coherent staging buffers into a separate device-local UBO. The target
buffer is never mapped. One descriptor, pipeline, image, readback buffer and
command buffer remain live across six submissions per size. Submissions
0/2/4 record uploads of good/alternate/good data, resetting the command pool
before 2/4 after fence completion; 1/3/5 resubmit the preceding executable command
buffer without recording. Every submission checks the exact expected pixel,
so returning to good data cannot hide an earlier stale result. Explicit
barriers cover previous resource use, transfer-to-uniform reads and readback.
`ubo-staged-validation` repeats both sizes under ICD VVL/SyncVal. This does
not cover descriptor templates, multiple queues, noncoherent staging or
simultaneous pending command buffers. Dynamic descriptors are exercised by
the separate dynamic/large cases, not combined with this staged path.

Rebuilt runs `20260907T041458-a0e027ef` (29854870) and
`20260907T041458-bb55971b` (KB2000) each report 83 PASS, 2 UNSUPPORTED.
Each native/frontend/ICD staged case logs twelve submissions across both
sizes: eight exact good pixels and four exact alternate pixels, with four
successful command pool resets. Both ICD VVL/SyncVal size variants report
zero errors. Existing capture/replay remains passing.


`ubo-template` uses Vulkan 1.1 core descriptor update templates for both UBO
sizes. Each template has a nonzero input byte offset: the first
VkDescriptorBufferInfo is a valid opposite descriptor, while the second is
the intended descriptor. Ignoring the offset therefore selects the wrong
color. One set/template and the same rendering resources survive six fenced
submissions per size: template updates good/alternate/good accompany command
pool reset/re-recording (reset only after the first recording); each recording
is then submitted twice. Every pixel is checked. `ubo-template-validation`
runs the same path under ICD VVL/SyncVal. This covers one uniform descriptor
entry via the core API; KHR aliases, arrays/stride traversal, multiple bindings,
push descriptor templates and update-after-bind remain unverified here.

Rebuilt runs `20260907T041926-f48b71fa` (29854870) and
`20260907T041926-c42297b8` (KB2000) each report 87 PASS, 2 UNSUPPORTED.
Each native/frontend/ICD template case logs twelve submissions and six
updates with payload offset=24 across both sizes, producing eight exact good
pixels and four exact alternate pixels. Both ICD validation size variants
report zero errors. Staged/dynamic cases and static capture/replay pass.


With capture tools enabled, `icd-capture-dynamic-replay` now records separate
good/alternate dynamic-UBO draws in `capture-dynamic/`. Each binding has an
uncaptured reference, a captured full-image readback and a replay resource
dump. The evidence checker reconstructs this fixture's effective descriptor
offset from its UpdateDescriptorSets base plus CmdBindDescriptorSets dynamic
offset; both vertex/fragment descriptor dumps must match the buffer, effective
offset, range, type and all 272 UBO bytes. Attachment before/after and final
copy must name the same image and agree with the full reference pixels.
`comparison.json` records descriptor base, dynamic/effective offsets and the
first attachment divergence for this fixed pair. The ordinary capture case
remains separately reported. This does not reconstruct multiple sets/bindings,
descriptor templates, command re-record histories or runtime generations, and
it does not identify the first erroneous draw of an arbitrary application.

Rebuilt runs `20260907T042356-82e88e05` (29854870) and
`20260907T042356-051c616f` (KB2000) each report 88 PASS, 2 UNSUPPORTED,
including validation/SyncVal and both capture variants. Dynamic evidence on
both devices has base=320, dynamic=640 / 320, effective=960 / 640 and range=272;
vertex/fragment dumps contain the exact good/alternate UBO and both attachment
comparisons first diverge at draw 61. Ordinary capture remains at draw 60.
Offline checks against the first run's evidence changed a bind offset by 64
and separately replaced the dump offset with the descriptor base alone;
both manipulations were rejected for both bindings. The originals were not
modified (`dynamic-offset-negative-check.log` records the checks).


Capture conversion now includes shader binaries. `shader_evidence.py` checks
that the bound graphics pipeline refers to the recorded successful creation,
its pipeline/set layout matches the bound descriptor set allocation, and its
render pass and two shader modules match the draw. Captured vertex/fragment
SPIR-V must exactly match the embedded arrays from the probe build's source
snapshot (whose hashes are checked against `probe-manifest.json`). The run
retains this reference under `shader-reference/`, validates captured modules
with host `spirv-val --target-env vulkan1.0`, and writes `spirv-dis` output.
Each binding's `pipeline.json` links capture module IDs, entry points, code
sizes, SHA-256, binaries, disassembly and interface decoration lines to its
pipeline/layout. Both capture variants require host `spirv-val` and
`spirv-dis`; their paths/versions are recorded in device metadata.
These are API-input modules, not driver-transformed shaders. Captured
pipelineCache=0 is reported as such, not treated as a driver cache key.
The association remains limited to this fixed single-pipeline fixture.

Rebuilt runs `20260907T043031-8348f666` (29854870) and
`20260907T043031-d81a25f9` (KB2000) each report 88 PASS, 2 UNSUPPORTED.
All ordinary/dynamic good/alternate capture variants validate the same
1832-byte vertex and 1092-byte fragment binaries against the build snapshot,
with matching pipeline/layout/module associations. VVL/SyncVal remains clean.
Offline mutations of the first run's pipeline layout and vertex module
(replaced with the fragment module) are rejected for all four captures;
`shader-negative-check.log` records these checks without changing originals.


Widget layout definitions and good/alternate data construction are separated
into widget_fixture.h, included in the probe source snapshot and manifest.
The helper returns owned 1232-byte storage; the small case uploads only its
272-byte prefix. Both layouts have compile-time size/offset checks. Shader
arrays, GPU call sequence and expected pixels remain unchanged.

Rebuilt runs `20260907T043548-b2be0f97` (29854870) and
`20260907T043548-895d9f1e` (KB2000) each report 88 PASS, 2 UNSUPPORTED.
All widget variants, validation/SyncVal and ordinary/dynamic capture evidence
pass after extraction; fixed attachment divergence remains draw 60 / 61.
This batch reorganizes fixture ownership and does not close additional
compatibility or arbitrary-application diagnosis gates.


`attachment_evidence.py` extends both capture gates with creation-to-copy
lineage: successful image/view/framebuffer creation, consistent owning device,
active render pass/framebuffer/view/image, format/dimensions, identity swizzle
and matching mip/layer/aspect/copy extent. Bind/draw/render/copy commands must
use the same submitted command buffer and follow creation/use order. The
comparison's `attachment_lineage` retains object IDs and creation indices.
This is limited to the single-image fixed fixture; aliases, multiple views,
subpasses, dynamic rendering, generations and WSI remain unverified.
Offline checks against `20260907T043548-b2be0f97` accept all four existing
captures and reject twelve copied-evidence mutations of framebuffer attachment,
view image or draw command buffer (`attachment-negative-check.log`).

Rebuilt runs `20260907T043953-23de5729` (29854870) and
`20260907T043953-70e52865` (KB2000) each report 88 PASS, 2 UNSUPPORTED.
Both ordinary/dynamic good/alternate captures pass the stronger attachment
lineage gate, with framebuffer=21, view=13, image=11 and command buffer=26
(capture-local IDs). Validation/SyncVal, shader and pixel checks also pass.


`sync-destroy` additionally checks busy normal/recursive/errorcheck mutexes.
The native/bionic fixture requires EBUSY with unchanged mutex storage, then
unlocks, locks/unlocks again and destroys successfully. This follows bionic's
explicit busy-destroy behavior, not a general POSIX portability guarantee.
Before the fix, `20260907T044320-8811de0f` reports native PASS and hybris FAIL:
the ordinary mutex returned EBUSY but its backing pointer was cleared. The
probe stops before touching discarded storage on that failure.
The hook now frees/clears only after successful host destruction and rejects
a failed shared-handle translation. Working process-shared mutex destruction
and concurrent destruction are not covered by this probe.
Source: https://android.googlesource.com/platform/bionic/+/master/libc/bionic/pthread_mutex.cpp

Rebuilt runs `20260907T044459-7e3295ea` (29854870) and
`20260907T044459-327bc0c1` (KB2000) each report 88 PASS, 2 UNSUPPORTED,
including validation/SyncVal and both capture gates. All three native/hybris
mutex types return EBUSY=16 with preserved=1, followed by successful
unlock/relock/final destroy. Common's 130 defined dynamic exports are unchanged.
The old comparison stops at the first failed normal mutex; it does not
separately reproduce old recursive/errorcheck behavior.


The legacy `pthread_mutex_lock_timeout_np` hook now uses CLOCK_MONOTONIC
via pthread_mutex_clocklock and maps ETIMEDOUT to EBUSY, matching bionic's
legacy contract. The bionic DSO imports this symbol for a hybris-only check
inside `cond-clock`: acquire an unused static mutex with zero timeout, wait
100ms on the held normal mutex, require EBUSY and at least 90ms elapsed, then
unlock/reacquire/unlock/destroy. No wall-clock adjustment is performed.
The API is absent from LP64 native bionic, so this is not an AArch64 native
comparison or 32-bit ABI validation; shared mutexes also remain unverified.
Before the fix, `20260907T044806-0401322d` returned ETIMEDOUT=110 after
100664271ns instead of EBUSY=16.
Source: https://android.googlesource.com/platform/bionic/+/63860cb/libc/bionic/pthread_mutex.cpp

Rebuilt runs `20260907T044949-b035cb87` (29854870) and
`20260907T044949-b9115f54` (KB2000) each report 88 PASS, 2 UNSUPPORTED.
The legacy mutex probe now returns EBUSY=16 after 103955677ns / 100934062ns,
with acquisition/reuse/cleanup passing. Validation/SyncVal and both capture
evidence gates pass. Common's 130 defined dynamic exports remain unchanged.


`mutex-monotonic` covers the API-28 pthread_mutex_timedlock_monotonic_np
entry, newly registered in the hybris hook table. It shares mutex translation
with ordinary timedlock but supplies CLOCK_MONOTONIC to the host. Native
resolves the platform entry; hybris executes an actual bionic fixture import.
The probe obtains an unused static mutex, waits against a 100ms monotonic
absolute deadline while held, requires ETIMEDOUT and at least 90ms elapsed,
then unlocks and acquires again with the expired deadline and with a null
deadline before cleanup. Null deadlines route to ordinary blocking lock;
this probe checks the uncontended null case.
This does not cover PI/shared mutexes, wall-clock jumps or all timedlock
validation cases. The legacy millisecond API continues to require EBUSY.

Final rebuilt runs `20260907T045649-a2754b94` (29854870) and
`20260907T045649-f05cab0e` (KB2000) each report 90 PASS, 2 UNSUPPORTED,
including native/hybris monotonic mutex, legacy timeout, VVL/SyncVal and both
capture gates. Native/hybris deadlines expire after approximately 100ms and
return ETIMEDOUT=110; expired and null deadlines acquire the unlocked mutex.
The new hook is hidden; the common defined export set remains 130 entries.


`rwlock-monotonic` exercises the newly hooked API-28 monotonic read/write
lock functions. Native resolves the platform entries; hybris calls actual
bionic fixture imports. The owner holds a writer while a worker attempts
read, then holds a reader while a worker attempts write. Each worker uses a
100ms CLOCK_MONOTONIC absolute deadline and must return ETIMEDOUT after at
least 90ms. After joining and releasing the owner lock, expired and null
deadlines must acquire the now-free lock and allow clean destruction. The
hooks reuse existing rwlock translation, use host clockrdlock/clockwrlock,
and route null deadlines to blocking rdlock/wrlock. This does not prove
fairness, shared-lock behavior, clock-step handling or contended null waits.

Rebuilt runs `20260907T050101-b55ee16e` (29854870) and
`20260907T050101-253dd56b` (KB2000) each report 92 PASS, 2 UNSUPPORTED.
Native/hybris read/write worker waits return ETIMEDOUT=110 after about
100–102ms, and expired/null unlocked acquisition succeeds. VVL/SyncVal
and both capture gates pass. Common's 130 defined dynamic exports match
the prior build; the new hook functions have hidden visibility.
