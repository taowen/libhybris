# Standard loader, validation and capture

Setup and dated integration evidence for the optional standard-loader route. Window validation and capture are documented in [the WSI suite](../wsi/README.md); headless draw evidence is in [widget capture](widget-capture.md).

## Optional standard Vulkan loader

The build also produces libhybris-vulkan-icd.so.0. It calls the vendor HAL
directly through hybris, leaving dispatchable-object headers to the standard
glibc loader. This path is opt-in; no system ICD JSON is installed. Baseline
ICD cases in this runner remain headless. Wayland surface, swapchain and present
checks live in the [separate WSI probe](../wsi/README.md) with `--icd-hal`.
See [adapter contract](../../hybris/vulkan/icd/README.md).

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
is explicit. Select individual ICD cases with repeated `--case`; omitting that
option runs the applicable expanded suite. The initial eight-case route covered
fill/readback, dispatch, lifecycle, unload/thread exit, caps, widget pixels and
direct linking.

Verified runs: 29854870 20260907T001046-6fb3cd47 and KB2000
20260907T001144-6f996d48, each 29 PASS / 2 UNSUPPORTED (desktop GL).
The ICD mappings contain the standard loader and vendor HAL, without Android
libvulkan. These results do not verify WSI, Vulkan 1.1 workloads, validation,
capture/replay, or complete physical-device command coverage.


## Standard validation layer

Build the pinned AArch64 layer with:

    bash tools/build-validation-layer.sh

This includes the dependency fix needed for grouped shader decorations; see
[the source, patch and original hang](spirv-groups.md). No validation rule is
disabled. It does not run automatically during ordinary builds or smoke runs.

Add its library, layer JSON and build manifest to the standard-loader command:

    --validation-layer tests/baseline/build/validation-build/install/lib/libVkLayer_khronos_validation.so
    --validation-manifest tests/baseline/build/validation-build/install/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
    --validation-build-manifest tests/baseline/build/validation-build/install/manifest.json

The initial runs below used Debian vulkan-validationlayers 1.4.309.0-1 from the
fixed snapshot. `tools/fetch-validation-layer.sh` retains that SHA256-verified
package for comparison; its grouped-decoration hang is still present.

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

## Mali loader workaround

The current known-build hook is automatic. `--icd-mali-loader-quirk 0`
provides an explicit negative control; the bare flag retains its historical
explicit-enable behavior. [Initialization and current regressions](../../docs/mali-mmud.md)
cover both EGL/Vulkan orders. The following runs used the earlier opt-in policy.

Mali MMUD opt-in (2026-09-07): `--icd-mali-loader-quirk` requests the
[build-id-scoped loader-inspection workaround](../../hybris/vulkan/icd/README.md#inspected-mali-mmud-compatibility).
It records the requested option, actual property adjustment and build-id,
without changing the Android system property. X300 full run
`20260907T065437-096b67ab` is 93 PASS / 4 UNSUPPORTED / 2 CRASH / 2 FAIL;
Redmi `20260907T065438-27c734a0` is 96 PASS / 4 UNSUPPORTED / 1 CRASH.
Both capture/replay gates and the non-template widget validation workloads
now pass on X300; core11, template/template-validation and native-groups remain
open. Same-build opt-out `20260907T065418-7b030d21` still crashes at pipeline
creation. This option was disabled by default and was not a claim of general Mali
compatibility. No unit-test suite was added; common's 130 export names are unchanged.

## ICD version discovery

ICD manifest version (2026-09-07): the new standalone `icd-version` case directly
queries the adapter/HAL before provisioning driver.json. It is automatically
included when selecting any ICD case. The version, output and mappings are
retained; discovery failure stops dependent cases. This corrects the old
hard-coded 1.0 manifest, which prevented loader version discovery and downgraded
requests reaching the driver. It does not change the workload's requested API
or enable extra extensions to hide a missing core implementation.

Final full runs with `--icd-mali-loader-quirk`: X300
`20260907T070031-042f6354` and Redmi `20260907T070032-2711e780` each have
**97 PASS / 4 UNSUPPORTED / 1 CRASH** (native-groups). Actual ICD instance versions
are 1.3.305 and 1.1.128 respectively. Core11 transfer, template/validation and
both capture/replay gates now pass on X300. The driver-specific opt-in was
required by those builds. Missing-HAL run `20260907T070107-1a7bcb32` correctly reports
icd-version FAIL without starting dependent cases. No unit-test suite was added.
