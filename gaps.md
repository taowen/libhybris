# libhybris graphics compatibility and rendering diagnostics gaps

Research date: 2026-09-06. Initial libhybris baseline:
`dcc3588262a2b5181a15998c63d5e7e684ef2631`.
Scope: extending this repository into an in-process GLES, desktop OpenGL, and
Vulkan compatibility stack, learning from Vortek and Gladio without copying
their command IPC.

Originally a research proposal, this document now serves as an ongoing acceptance
checklist. See [implementation status](IMPLEMENTATION.md) for progress. The target
architecture and complete compatibility layer remain unfinished. Mali Blender
has real capture, repair, and modeling-workflow evidence; complete application
acceptance and resolution of the other Turnip failure are still outstanding.

## Current product stack and consolidation status (2026-09-09)

The current compositor entry is the installed `io.taowen.arlinux.debian`.
Arlinux builds Xwayland; libhybris builds clients only. Names such as
`io.taowen.hybriswsitest` below belong to dated historical records, not current
fixtures or acceptance instructions.

Product Mesa uses the `taowen/mesa` WSI fork at `bfe5f4ce`. Official upstream
`c3b008c1` below is historical offscreen evidence; desktop-gl now directly reuses
the product Mesa build and libraries. Frontend Wayland/X11 window plugins, the
Vulkan plugin-loading layer, and PRESENT_SOCKET were removed. Native-window
implementations required by the ICD moved to `vulkan/icd/`.

Independent log review corrected the old desktop-gl claim of no validation
errors; do not carry it forward as a passing result. Both product Vulkan
backends now share window probes, standard layers/capture, and screen-evidence
gates; see [shared window evidence](tests/wsi/product-backends.md). Application
and failure-race gates remain open. Consolidated items and remaining differences
are listed in the [product stack checklist](docs/stack-consolidation.md).

Generic compatibility now lives in `VK_LAYER_HYBRIS_compat`. The HAL ICD and
Turnip share this layer, lifecycle diagnostics, backend deployment, and the
application observation entry. Downstream application VVL on Mali exposed
`VUID-RuntimeSpirv-OpVariable-08746`: cleaned-up vertex outputs no longer matched
geometry-input BuiltIn blocks. The automatic path now preserves interface
declarations; Blender startup has zero downstream VVL errors on both backends.
See [shader-interface evidence](docs/blender-shader-interfaces.md).

Turnip's forced BC image case still has 12,524 comparison errors. Startup
screenshots and before/after captures do not replace modeling, saving, resizing,
or replay acceptance. Subsequent staged Blender runs on both backends completed
scripted modeling, saving, fullscreen/restore, and reopening in one process.
Final screenshots showed the model and VVL reported zero errors; see Arlinux's
[workflow evidence](../../tests/blender/README.md). Formal installation
integration, arbitrary resize stress, long-term use, and capture replay remain
unverified.

Mali Zink teapot device creation was fixed by converting ClipDistance requests
in the Features2 input chain. Existing feature probes and product GLX/Wayland
presentation/resize pixel gates passed on both devices after reinstallation;
see [device-feature chain evidence](tests/baseline/device-features.md). This
does not cover Blender startup through Zink; Blender acceptance still uses
direct Vulkan only.

## Historical official-Mesa transition (2026-09-07)

Mesa was switched to official upstream `c3b008c1`, removing dependence on the
Mesa fork, direct Gallium Freedreno KGSL, and custom Zink vertex emulation.
Previous fork passes are historical, not coverage of that upstream version.
Original Turnip plus Zink passed two EGL/GLX rounds on Redmi, with 38 matching
images, 44 validated SPIR-V modules, SSBO limits 16/16/16, and zero SyncVal
errors. On Mali, upstream Zink rejected core 3.3; the core 3.2 packed-attribute
case failed. Blender displayed its OpenGL 4.3 requirement dialog and its startup
script did not run. Blender was not installed on Redmi. See [upstream Mesa
evidence](tests/desktop-gl/README.md#official-upstream-results-2026-09-07).

Subsequent work retained upstream Mesa. The user accepted Zink incompatibility
with Blender, so Blender startup/rendering through Zink is no longer an
acceptance requirement. Those failures remain observations. Other applicable
driver capabilities, standard interfaces, and regressions still require
validation; restoring private Mesa patches or increasing advertised values
does not establish completion. G07/G08/G13 remain open. G10 is closed because
the product desktop-GL frontend is Zink.

## 1. Conclusions and boundaries

libhybris connects glibc programs to Android GPU drivers, but is not yet a
Vortek-style Vulkan semantic compatibility layer. Mesa/Zink is the desktop-GL
frontend, not Gladio; G10 is closed. The main gaps between basic tests and
correct complex-application rendering are:

1. Complete, verifiable API dispatch and object management without wrapper bypass.
2. Arbitrary application-level validation and capture. Standard-loader headless
   widgets and window probes already use VVL/GFXReconstruct; that integration exists.
3. Evidence linking each draw/submit to descriptors, memory contents,
   attachments, and the final presentation buffer.
4. Format, shader, and synchronization compatibility constrained by real
   capabilities and protected by pixel regressions.
5. Application windows (teapot/scene) and Vulkan probes remain different
   workloads. Shared gates do not yet cover all races or real advertised-usage operations.

Window submission itself is no longer a gap: the product path is Zink plus the
standard loader → ICD/Turnip WSI → TAWC-DRI / android_wlegl → anlabwc. Do not
introduce a second cross-process present path or a hybris EGL desktop-window plugin.

Prioritize the GPU API, compatibility, and diagnostics layers while retaining
the Android loader, libc hooks, and TLS fixes as a regression-protected base.
Rewriting TLS and graphics compatibility simultaneously obscures failure sources.

Measure compatibility by correctly running applications and verified semantics,
not inflated Vulkan/GL versions, extension counts, or successful window creation.
Emulation has a performance cost. Report unsupported functionality when the
hardware lacks it and no correct implementation exists.

## 2. Established evidence

### 2.1 Standalone baseline on device 29854870

[tests/baseline](tests/baseline/README.md) uses no APK, rootfs, Xwayland, or
compositor. Identical C programs are built for bionic and glibc and run as the
Android shell UID. This does not establish identical permissions or namespace
behavior under an application UID.

| Item | Native Android | glibc + hybris |
|---|---|---|
| EGL initialization | 1.5 | 1.5 |
| GLES 2 / GLES 3 context requests | Both succeed; actual result is GLES 3.2 | Same |
| Offscreen clear, shader compilation/linking, triangle pixel readback | Pass | Pass |
| Vulkan instance/device, buffer fill, fence, 1024-word readback | Pass | Pass |
| Vulkan extension enumeration | Same as hybris | Same as native; null WSI |
| EGL desktop-OpenGL context | Unsupported | Unsupported |

Device model `M2012K11AC`, Android 13 / SDK 33. The driver reports Adreno 650,
Vulkan 1.1.128, driver `0x801f6000`, and GLES 3.2 V@0502.0. Use query results,
not model-name inference. Queries found BC=false, ETC2/ASTC=true,
geometry/tessellation=true, float64/int64=false. These are capabilities, not
verified corresponding draws. The extension list lacks timeline semaphores,
dynamic rendering, synchronization2, descriptor indexing, and device fault; it
does not describe the other two devices with Blender failures.

The Vulkan workload requests 1.0. Transfer, widget graphics UBOs, and two-thread
device/fence lifetimes were added, but compute shaders, textures, AHB, WSI, and
CTS remain uncovered. Do not claim Vulkan 1.1 or complete GLES 3.2 validation.
See [initial baseline evidence](tests/baseline/smoke-results.md). Raw JSON/logs
remain in ignored `tests/baseline/build/results/`. Complete applications were
not rerun in that round.

### 2.2 Initial source audit, not old installed outputs

This describes the initial `dcc3588` source snapshot, not current HEAD.
See [implementation status](IMPLEMENTATION.md) for subsequent implementation
and acceptance boundaries. Current `hybris/vulkan/icd/instance.c` and `device.c`
retain instance/device resolvers, generations, and allocators and wrap creation,
destruction, and physical-device enumeration. Those records do not establish
complete command compatibility state or semantics; G02/G03 remain open.
HEAD removed `hybris/vulkan/platforms/`; window WSI lives in `vulkan/icd/`.
An installed `vulkanplatform_x11.so` was a stale output, not a source capability.

| Module | Evidence | What it does not establish |
|---|---|---|
| [Vulkan wrappers](hybris/vulkan/vulkan.c) / [exports](hybris/vulkan/vulkan_exports.c) | Separate export trampolines; GIPA/GDPA intercept a few WSI functions and forward others | Every new compatibility transform covers static linking, dlsym, GIPA, and GDPA |
| Vulkan platform build (`hybris/vulkan/platforms/Makefile.am` at `dcc3588`, absent at HEAD) | common/null/wayland in the snapshot; wrappers reject Xlib/XCB surfaces | An old installed `vulkanplatform_x11.so` reflects past or present source support |
| [GLES wrappers](hybris/glesv2/glesv2.c) | GLES exports and Android-library bridging | Desktop OpenGL core/compat, GLX, or complete GL-to-GLES translation |
| EGL desktop-window plugins | Removed; `ws_init` rejects wayland/x11 | Current hybris EGL windows are a product path |
| ICD Wayland/X11 WSI | `vulkan/icd/` submits through android_wlegl / TAWC-DRI | Shared window tests pass, but failure races remain open |
| [Loader bridge](hybris/common/linker_bridge.c), [libc hooks](hybris/common/hooks.c), [synchronization bridge](hybris/common/bionic_sync.c), [TLS notes](TAWC_FORK.md) | Separate Android linker, libc/thread bridge, ARM64 TLS thunks | Compatibility with every Android version and vendor library |

Use clean staging and record actual loaded build-ids/SHA256. Installed filenames
alone cannot establish source functionality.

### 2.3 Lessons from Vortek and Gladio

Cross-process command IPC, PRESENT_SOCKET, and in-APK GPU hosts were removed
from the product tree. Integration is in-process: Zink plus the standard loader
→ hybris ICD or Turnip; TAWC-DRI / android_wlegl deliver windows to anlabwc.
Historical reference source is in workspace `x11-glibc-apk`; see [source scope
checks](docs/vortek-scope.md). It is an algorithmic reference, not a product path
or CTS oracle.

| Reference | Observed capability | Product status |
|---|---|---|
| Vortek ShaderInspector | Scaled vertices / conditional SPIR-V handling | In-process ICD `compat/`, without RPC objects |
| Vortek TextureDecoder | BC decoding and replacement uploads | In-process ICD `compat/` |
| Timeline wait transport | RPC waiting through eventfd | Not ported; native core/KHR timeline passthrough |
| Gladio host / command ring | GLES command IPC | Removed; desktop GL uses Zink |

Vortek packaging does not improve API semantics. Do not reintroduce command rings
or sockets.

## 3. Gaps exposed by two Blender failures

References to `../../docs/` below are historical investigations in the parent
Arlinux checkout, unavailable in a standalone clone. Necessary summaries are
retained here; these are not newly verified results.

### 3.1 Mali: correct cube, white widgets and magenta strips

Source: [BLENDER_HYBRIS_MALI_WIDGET_UI.md](../../docs/BLENDER_HYBRIS_MALI_WIDGET_UI.md).
Device `10AFA31610002QH` / Mali-G1-Ultra, Blender 4.3.2, historical experimental
Vulkan X11 path. Its compatibility transforms and X11 WSI were later reverted.

Recorded observations:

- A cube using 80/96-byte push constants rendered. The 272-byte std140 widget
  UBO exceeds the device's 256-byte push-constant limit; an instanced widget is
  approximately 1.2 KB. Inflating advertised limits cannot solve this.
- Widget layout: parameters@0, MVP@192, checkerColorAndSize@256,
  srgbTarget@268. Copy/alias experiments read back real rectangle/matrix data;
  assuming the UBO always contains zeros is no longer justified.
- Correct data at `vkUpdateDescriptorSets` does not prove the draw uses the same
  set/buffer/range/layout or that its target attachment is composed correctly.
- Magenta strips were recorded as a texture-path error and require separate
  validation from widget UBOs.
- A `vkCmdBeginRenderingKHR` wrapper resolved to the wrong trampoline/no-op.
  Real function pointers were also unconfirmed in an indexed-draw experiment.
- Illegally removing clip capabilities/members caused a GPU fault. Skipping
  transforms for UBO shaders did not change the image. An unchanged screenshot
  is one experimental result, not a mathematical exclusion under all conditions.

Required reproduction and observations:

1. Explicitly check bool/int, mat4 strides, and descriptor dynamic offsets for
   272-byte and approximately 1.2 KB UBOs. Encode selected fields into pixels or
   an SSBO to prove the shader reads correct values.
2. For 12 vertices / 18 indices and a `gl_VertexIndex` widget, test direct UBOs,
   staging copies, descriptor updates/templates, rerecording, and resubmission,
   with expected pixels at every step.
3. At each draw, record the effective pipeline/layout, set bindings, buffer
   generation, offset/range, push constants, and target view/subresource.
   UpdateDescriptorSets logs alone are insufficient.
4. Read back the widget-region FBO, composed image, and submitted AHB to locate
   the first pixel divergence. Enable diagnostic readback on demand with correct
   synchronization.
5. Associate original/transformed SPIR-V, reflection, and cache keys with the
   pipeline; make actual backend function resolution inspectable.

Descriptor binding, shader compilation, resource lifetime, UI FBO/blit, or a
combination remain possible causes. Do not assume a UBO fix as the conclusion.

### 3.2 Adreno / Turnip: device lost after View3D redraw

Source: [BLENDER_TURNIP_DEVICE_LOST_INVESTIGATION.md](../../docs/BLENDER_TURNIP_DEVICE_LOST_INVESTIGATION.md).
OnePlus PJZ110 / Adreno 830v1, Turnip 26.3.0-devel, Blender 4.3.2 Vulkan.
**This is not evidence of a hybris/vendor-driver failure.** It motivates real
workload and diagnostic regressions.

Basic readback, first drawing, and View3D-shaped probes pass, while orbit/redraw
may lose the device. Large RGBA16F plus D24S8 with repeated LOAD/copy/blit did
not reproduce it. Missing combinations include bindless descriptors, multiple
texture accesses, complex shaders, packed SNORM, instanced vertex input, and
repeated attachment/resource reuse. RD/cffdump prefetch/constlen values describe
Turnip compiler outputs, not cross-vendor Vulkan requirements. A GPU bindless
bit does not identify an application descriptor-indexing feature.

Incrementally reproduce these combinations in legal Vulkan source probes,
retaining each diff, input, and pixel/crash result. Use API replay to narrow the
failure. RD/cffdump is optional Turnip evidence; manually trimmed GPU packets
are not final tests. A `vkWaitForFences` error is a detection point, not
necessarily the trigger. Correlate recent submissions, actual completion
sequence numbers, and the last known correct attachment.

## 4. Proposed target architecture

```text
Desktop GL app -> Mesa Zink --+
                             +-> Standard loader -> hybris ICD or Turnip
Vulkan app ------------------+          |
                                        +-> Wayland: android_wlegl -> anlabwc -> Surface
                                        +-> X11: TAWC-DRI -> Xwayland -> android_wlegl -> anlabwc
GLES app -> hybris GLES (not the product window path)
```

API compatibility must not know window XIDs or compositor-private sockets.
WSI must not transform shaders or misrepresent GPU features. AHB is a shareable
buffer, not a command protocol. `android_wlegl` and `TAWC-DRI` are extensions
both endpoints must implement; core Wayland/X11 does not automatically support
AHBs. Standard DRI3/dma-buf integration requires separate format, modifier,
synchronization, and handle evidence. An AHB FD is not automatically a dma-buf.

### 4.1 Validate Vulkan loader / ICD decisions first

Prefer retaining the standard glibc Vulkan loader, adding a hybris ICD adapter,
and implementing composable compatibility modules/layers so standard validation
and capture have normal entry points. The ICD must implement negotiation,
dispatchable objects, physical-device procedure queries, and surface/WSI
contracts; adding JSON for the current `libvulkan.so` is insufficient. See the
[Khronos loader-driver interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md).

The preliminary headless path, standard glibc loader → optional hybris ICD →
vendor Vulkan HAL, passed eight basic probes on two Adreno 650 devices.
Standard validation and offscreen widget capture/replay passed. Standard-ICD
window VVL and GFXReconstruct virtual-swapchain capture/replay passed on
OnePlus 8T and Mali, without establishing arbitrary-application or full WSI
conformance. The Android loader is downstream of the bridge; verify handle and
dispatch ownership across both loaders to prevent recursive libvulkan loading.
Resolve Android libraries through explicit paths/namespaces.

A short-term approach can unify current frontend dispatch before an adapter
spike. A replacement `libvulkan.so` still requires measured layer chaining;
`VK_LAYER_PATH` alone guarantees nothing. Application-side and transformed
backend-side validation require separate boundaries. Upstream layers do not
automatically observe internal ICD transforms, and a glibc layer cannot simply
be loaded by the bionic loader.

### 4.2 OpenGL strategy

The 2026-09-07 decision selects Mesa/Zink for desktop GL, reusing Mesa's GL
state machine, GLSL, and core/compat frontend. This is a maintenance/coverage
choice, not a same-application comparison proving general superiority over
Gladio or a Blender pass. Adreno uses Zink → Turnip where the pinned driver and
device tests support it. Mali and other devices use Zink → hybris Vulkan →
vendor driver, with compatibility for demonstrated gaps. Qualcomm retains the
vendor-driver path as an option; libhybris is not limited to Mali.

Choose the vendor driver or Turnip by measured device capability and workload.
One vendor driver lacking timeline support does not mean libhybris excludes
that GPU vendor. The common elements are the GL frontend and Vulkan interface;
Turnip is not a cross-GPU backend. Retain useful Gladio application experience
as regression references and GLES for existing cases or devices without the
required Vulkan capabilities, without building another generic desktop-GL frontend.

The Adreno build removed direct Gallium Freedreno GL. The parent's
`tools/build/mesa.sh` selects Gallium `zink` and Vulkan `freedreno` (Turnip);
`freedreno-kmds=kgsl` selects Turnip's kernel interface. Turnip still uses shared
Freedreno compiler/device code. Upstream offscreen EGL/GLX passes do not
establish real-window presentation, resize, or same-application performance.
Validate Zink emulation with actual rendering and validation, not elevated GL
versions or resource counts intended to bypass application checks.

| Route | Value | Limitations |
|---|---|---|
| Mesa/Zink → hybris Vulkan | Modern desktop GL with the complete GL state machine/compiler | Depends on the pinned Mesa version's feature, format, and limit requirements, not just Vulkan version |
| Separate GL-to-GLES frontend informed by Gladio | Legacy GL/compatibility profiles and insufficient vendor Vulkan support | Large fixed-function, GLSL, texture/FBO, GLX/context implementation effort; no small-change promise for modern Blender |
| GL4ES | Comparable legacy-GL reference | Targets GL 1.5/2.1; not a complete modern desktop-GL replacement |
| ANGLE | GLES implementation and shader/driver-workaround reference or optional layer | Not a desktop-GL implementation; cannot invent missing underlying Vulkan functionality |

Pin Mesa and use Zink requirements/profile checks to report gaps by target GL
version. Its documentation lists multiple extension requirements; Vulkan 1.3+
alone is not sufficient evidence. References: [Zink](https://docs.mesa3d.org/drivers/zink.html),
[GL4ES](https://github.com/ptitSeb/gl4es), [ANGLE](https://github.com/google/angle).

Device 29854870's basic GLES path supports GL-to-GLES experiments, while its
Android vendor Vulkan driver lacks several modern extensions. With upstream
Turnip, current Zink EGL/GLX probes report GL 4.6 and passed the 38 image
comparisons above. This covers specified workloads/drivers, not complete
GL 4.6 compatibility or CTS.

Same-day native-3/hybris-3 baselines passed on three devices with matching GLES
capability queries and GL error=0. X300 (`20260907T131453-7e930597`) has vertex
SSBO/image limits 0/0 and fragment/compute SSBO limits 35/35. Redmi 29854870
(`20260907T131453-2abee58b`) and OnePlus KB2000 (`20260907T131453-7a91c77a`) have
vertex SSBO/image limits 4/4, fragment SSBO limit 4, and compute SSBO limit 24.
GLES passthrough therefore cannot meet Blender's current 12-SSBO-per-stage
check. This does not prove Gladio translation cannot run other applications,
or establish desktop-GL, complete GLES, or performance acceptance on those devices.

## 5. Gap checklist and acceptance criteria

P0 establishes a base before compatibility enhancement; P1 directly affects
target applications; P2 expands coverage after basic usability. Open checklist
items must not be described as implemented features.

### G01 / P0: independent builds and output provenance

Require standalone builds with pinned headers/compiler/dependencies and runtime
manifests recording actual ELF hashes/build-ids, drivers, devices, environment,
and quirks. Clean installations must exclude stale platform libraries.

**AArch64 baseline verified:** the repository pins its base digest, Debian
snapshot, and Android-header commit. Source/header snapshot hashes, container
IDs, package versions, compiler, and configuration are retained. Probes have
separate manifests. Runs record commands, driver strings, per-stage mappings,
deployment hashes, and post-run hashes of Android paths. Standalone checkouts
produced 21 PASS / 2 UNSUPPORTED on both devices; see [build evidence](tests/baseline/build.md).
Mapping snapshots are not full-lifetime tracking or memory-page verification.

### G02 / P0: dispatch, aliases, and per-object function tables

Generate coverage from pinned vk.xml; run identical tests through
link/dlsym/GIPA/GDPA. Test BeginRendering/KHR, Submit2/KHR, and other aliases
according to enabled capabilities. Unsupported symbols must obey the specification.

**Partial:** ordinary GIPA/GDPA queries retain backend instance/device results,
replacing only necessary frontend/WSI wrappers. Negative tests cover scope and
disabled extensions; unresolved direct exports produce diagnostics. A pinned
Vulkan-Headers v1.4.309 registry supplies scope/alias/provider data for 726
commands. Device runs record dlsym/GIPA(NULL)/GIPA(instance)/GDPA, checking 137
required core 1.0 entries and forbidden non-global/non-device scopes. Identical
fill/fence/readback executed through all four entries; enabled core 1.1/KHR
memory2 paths were verified.

Swapchain wrappers resolve through the actual device's GDPA and reject missing
functions. Wayland create/destroy resolve through the current instance, with
build-only verification. Disabled-extension direct exports no longer report
success. The optional standard ICD retains instance/device resolvers,
destructors, and unique generations; ordinary and core/KHR group physical-device
enumerations register their owning instance for creation/query/destruction.
The replacement frontend records device/queue/pool/command-buffer ownership
and dispatches four core/KHR rendering command groups through the actual GDPA.
X300 fixed draws passed all four frontend entries. Three core/KHR host timeline
command groups also resolve and execute for the supplied device. Complete
frontend resource/generation state, all aliases, and rendering/synchronization
semantics remain uncovered.

### G03 / P0: concurrency and object lifetime

Require regressions for parallel creation/destruction, repeated initialization
and dlopen, callbacks, thread TLS, and reused handles. Identify objects by
generation, never one process-global current device.

**Partial ownership model:** glibc keeps `libhybris-common` (`DF_1_NODELETE`)
until process exit. Common owns its Android linker plugin and never dlcloses it;
frontends hold their own glibc references. Callers of `android_dlopen` own vendor
objects, which frontend shutdown does not reclaim. Valid shutdown destroys
Vulkan devices/instances, drops frontend references, then exits the process.

The hook table's first qsort now publishes through pthread_once, preventing
sorting/search races from an unlocked sorted flag. Existing initialization
regressions pass, but internal lookup is not exported and the first-sort race
was not independently reproduced. Callback publication/query snapshots and
missing-symbol numbering use atomics; the miss-log switch uses pthread_once.
Replacing a callback does not wait for previous calls; callers retain lifetime
responsibility. Evidence consists of review and existing regressions, not an
independent callback-replacement race reproduction.

`ENSURE_LINKER_IS_LOADED()` uses `pthread_once`. Review found no bundled-linker
initialization path reentering public `android_*` wrappers; new callbacks must
preserve that constraint. `_hybris_hook_dlerror` directly calls `_android_dlerror`
to avoid reentry during once initialization.

Baseline `life` covers two devices, destruction/recreation, repeated dlopen,
and two-thread creation/destruction, but not first entry. `init` covers two
threads making their first `android_dlopen` simultaneously. `tls` lets the main
thread close a frontend after a worker completes Vulkan creation/destruction,
then exits the worker. Bionic C++ DSOs using both emulated TLS and native
TLSDESC verify initial values and destructors. Native TLS initially read zero
on glibc workers; the Q linker's static TLSDESC resolver now initializes the
thread and replays .tdata. After closing the main reference, each destructor
runs exactly once on its owning thread with the correct value; three rounds
passed. This does not prove compatibility-allocation cleanup, IE TLS/signal
behavior, or static-slot reclamation.

Run `20260906T233143-5f5b13ed` passed hybris `init`/`tls`/`unload`. Native `init`
has no corresponding implementation and is no longer scheduled; failure to
load hybris common is FAIL. Injecting failure into the second pthread_create
releases the barrier, joins the started thread, and exits 2. A zero `dlclose`
result does not establish unmapping; exit 0 does not prove reclamation; repeated
creation does not prove generation management.

The optional ICD's instance-generation table pairs four threads' 16 lifecycles
without leaks on both devices; each round requires four simultaneously live
instances. Application allocators complete three create/query/destroy rounds
without outstanding allocations and return OUT_OF_HOST_MEMORY when refusing
allocation on three paths. Standard-path failures may occur in the loader.
A direct-ICD probe confirms that refusal of the first state allocation calls
the allocator once and recovery leaves no lifecycle records. This does not
isolate every HAL allocation point or allocator-lock interaction. Allocation
callbacks must not call Vulkan commands.

Those older records cover instances only. The device table verifies generation,
parent instance, paired destruction, and no leftovers across 19 `life`
lifecycles, including reused raw device handles. Direct-ICD allocator probes
cover first device-state allocation refusal/recovery after ordinary and
core/KHR group enumeration. Further probes refuse enumeration-callback
allocations and recover on the same instance; all three paths on both devices
return OUT_OF_HOST_MEMORY, recover successful device lifecycles, and retain no
callback allocations per round. This covers adapter/HAL boundaries, not every
HAL allocation point. Resource state, some multi-GPU enumeration failures, and
concurrent enumeration remain open.

Two unshared GLES contexts now test independent buffer/clear state, exact pbuffer
pixels, main→worker→main migration, and three destroy/recreate rounds; native
and hybris pass on both devices. `vk-init` starts four threads together for
first enumeration/creation, then performs four instance query/destruction rounds
per thread. Platform initialization/global resolution use pthread_once; null
platform is device-tested, Wayland only build-tested.

Static Android mutexes now publish one backend pointer under a lock, supporting
four-byte alignment. A bionic DSO with 32 new locks and four-thread first-use
contention passes on both devices; old-common controls time out. Rwlocks also
publish one backend; 32 new locks verify four-thread write exclusion, concurrent
read ownership, and rejected try-write. Initial condition-pointer publication
is serialized; 32 new condition variables verify timedwait/signal/broadcast.
The old version also passed that round, so lost wakeups were not reproduced.

Without glibc /dev/shm, allocator/translation no longer dereferences null;
initialization of all three shared synchronization types returns ENOMEM. This
does not establish normal shared synchronization support. Two monotonic-cond
aliases now explicitly use CLOCK_MONOTONIC so 100 ms deadlines no longer expire
immediately. Relative waits use monotonic time and reject invalid nanoseconds,
negative seconds, and overflow; independent probes verify waiting and EINVAL.

Mutex destruction retains backend storage when the host returns EBUSY. Native
and hybris tests on both devices cover normal/recursive/errorcheck mutexes,
unlocking after failed destruction, relocking, and final destruction. The old
normal-mutex control reproduced EBUSY with cleared storage. Legacy mutex
lock_timeout_np uses monotonic time and maps timeout to EBUSY; imported bionic
DSO probes verify 100 ms waiting and reuse on both devices, reproducing old
ETIMEDOUT behavior. No LP64 native export exists; 32-bit ABI and wall-clock jumps
remain unverified.

An API-28 mutex_timedlock_monotonic_np hook has native/hybris controls for
monotonic absolute timeouts, expired deadlines, and acquiring idle locks with
null deadlines. PI/shared locks and cross-thread null-deadline waits remain
untested. API-28 rwlock monotonic read/write hooks have holder/waiter controls
for approximately 100 ms timeouts and acquisition after release with expired/null
deadlines on both native/hybris paths. Normal shared synchronization, other
condition clocks/destruction, rwlock fairness, and remaining timeout boundaries
are still unverified.

Shared GLES2 contexts verify buffer size, exact texture pixels, binding
isolation, and access after creator-context destruction over three rounds on
both native/hybris devices. Two unshared contexts execute independent
clear/readback concurrently on their own current threads, then migrate back to
the main thread for checks. Independent contexts also draw blue/yellow fragment
programs concurrently with exact pixels and no program/vertex-attribute rebinding
after migration. These tests do not provide a generation object table or prove
overlapping GPU execution.

### G04 / P0: standard loaders, layers, and tools

Require validation to catch a known invalid test, zero new errors for a valid
test, then one matching captured/replayed frame before expanding to applications.

**Partial:** the optional ICD directly connects vendor HAL; eight standard glibc
loader headless cases pass on both devices without rewriting dispatch headers
or removing creation chains. Standard glibc VVL 1.4.309.0 reports zero ERROR
for legal instance/device/buffer lifetimes and the exact zero-length-buffer
VUID, stopping that call before the vendor. Fixed-widget upload/draw/readback/
destruction uses SyncVal; both bindings produce expected pixels with zero ERROR.
Pinned GFXReconstruct captures/replays correct/incorrect 16×16 RGBA8 bindings;
uncaptured, captured, and replayed full images match byte-for-byte. Offscreen
capture has no present frame and cannot close the WSI/application frame gate.

The standard ICD uses existing `window_owner` Wayland surfaces and imports
window buffers through `VK_ANDROID_native_buffer` for local `VK_KHR_swapchain`
support with finite acquire timeouts, release fences, and queues. HAL-owned WSI
is still rejected. Optional runner switches enable standard VVL/GFXReconstruct.
OnePlus 8T `20260907T225316-dcafbe9f` and Mali `20260907T225316-bf0c6ea9` report
`WSI_VALIDATION errors=0` through create/render/resize/retirement/destruction and
boundary probes. Virtual-swapchain replay with `gfxrecon-replay --swapchain virtual`
performs 24 copies; six live readbacks match byte-for-byte in
`20260907T230116-0e424415` / `20260907T230116-2c517ce0`. This is not a second
present path and needs no image alias. The replacement frontend has single-window
Wayland presentation evidence, but no integrated window capture/replay.

Grouped shader member names triggered an infinite decoration-expansion loop in
old VVL. A pinned-source validation-layer build changes only iterator advancement.
Grouped draws pass on both devices, invalid buffers still produce the exact
VUID, and source/patch/loaded-layer hashes accompany runs; see [grouped SPIR-V
and validation](tests/baseline/spirv-groups.md).

Targeted captures automatically prepare ICD-version/widget dependencies and
retain actual scheduling plus exit results from 16 capture phases. Ordinary/
dynamic binding images and shader/resource audits passed again on both devices.
Native-buffer foundations are verified: actual gralloc import, Vulkan/CPU dual
readback, release/acquire FD round trips, and an AHB borrowing interface.
Internal Wayland native-window creation/cleanup no longer depends on the Vulkan
loader; reusing it in the frontend passed surface lifetimes, three presentation
sizes, and screenshots on both devices.

Experimental FIFO swapchains fixed initial creation crashes, native-buffer
base-pointer conversion, old-image references, finite-timeout event reads, and
duplicate semaphore waits across swapchains. Actual presentation/readback/
screenshots, retirement/timeouts/allocation boundaries, window VVL, and virtual
capture/replay pass on OnePlus 8T/Mali. Missing destruction-time diagnostics were
reproduced and fixed; original capture/replay images are retained. Pinned tools
cannot run validation and capture together or replay injected allocation
failures in boundary probes; the runner rejects both combinations early.
See [validation/capture review](tests/wsi/validation-capture-review.md) and
[swapchain evidence](tests/wsi/swapchain-review.md). Image aliasing, complete
device-group/protected modes, and arbitrary application captures remain open.
See [native-buffer evidence](tests/baseline/native-buffer.md). Results from the
old handwritten validation chain remain withdrawn.

### G05 / P0: capability declarations and implementation

Keep features/features2, properties/limits, extensions, format/image-format
queries, and CreateDevice enables consistent. Record raw/effective differences
and reasons; never retry by dropping the entire pNext chain.

**Partial:** baseline `caps` verifies exact rejection of unadvertised features
and unknown extensions, plus null GDPA for disabled extensions. Native/hybris/ICD
comparisons retain 325 named query values: 174 core feature/limit/sparse values,
device identity, device extensions, and ten formats. Frontends match on both
devices. The direct HAL ICD has six buffer/present extension differences; this
does not establish stack-wide equivalence.

Five core 1.1 features2 structures, consistency of 55 core fields, same-chain
device enablement, and exact refusal of false float64 passed on three paths on
both devices. Six core 1.1 properties2 structures have whole-chain/per-structure
controls and named legacy-properties comparisons; 198 recorded features2/
properties2 values match across all three paths on both devices.

Scaled-format policy is a separate module with bounded, default-off per-format
records of raw/integer-fetch/effective flags and reasons. Adreno missing-format,
Mali forced-transform, and Mali native-preservation decisions have independent
native queries and actual draw controls; see [format-policy evidence](tests/baseline/scaled-format-policy.md).
Other extension query chains, complete format creation, and reasons for other
compatibility transforms remain incomplete.

### G06 / P0: draw-to-resource-to-image diagnosis

Require Mali-shaped UBO binding/layout/content/attachment evidence that locates
the first incorrect draw after injecting a bad binding. **Partial:** fixed
widgets cover UBOs, staging, templates, ordinary/dynamic/multiple descriptors,
capture, and API-input shader correlation. See the G06 evidence below.
Arbitrary-application first-bad-draw diagnosis, runtime generations, and WSI
attachment lineage remain unfinished.

### G07 / P1: Vulkan format compatibility

Each supported format needs golden/reference pixels and consistent query,
creation, view, copy, readback, mip/layer/subregion behavior. Cover BC6H/BC7
separately; never advertise unimplemented support.

**Experimental subset:** the standard ICD implements integer fetch plus SPIR-V
compensation for 12 R/RG/RGBA 8/16-bit scaled vertex formats. Redmi Adreno's
missing vendor formats pass fixed pixels when enabled; Mali passthrough and
forced-transform controls pass. Mixed native-float and signed/unsigned scaled
columns in float32 matrices/fixed arrays passed four aggregate shapes with
pixel/VVL controls on Adreno/Mali. Static instance-divisor chains are preserved;
divisors 1/2/3 pass on both. Adreno zero-divisor and nonzero-firstInstance pass;
Mali reports those capabilities false and is excluded from that coverage.
See [instancing evidence](tests/baseline/scaled-instancing.md). This is not
complete format support.

BC1–BC5/BC7 image/view/copy paths are default-off, with capability-limited queries
and creation. Redmi/Mali probes cover 12 formats, mip/layer/subregion, GPU writes,
sRGB/R-B swizzles, exact native RGBA8/R16/RG16 sampling controls, and resource
reset. BC4/BC5 also cover 16-bit UNORM/SNORM, linear filtering, and default
border channels. A new BC1 RGB border probe led to a native RGB8 fix on Mali;
Redmi remains explicitly FAIL. See [RGB8 records](tests/baseline/bc-rgb8.md).
BC7 kernels and Mali images passed an independent reference; Redmi native BC7
sRGB filtering still fails the CPU reference. See [BC7 records](tests/baseline/bc7.md).
textureCompressionBC remains false. BC6H, complete mutable/sparse/external/alias
semantics, and CTS remain unverified. See [image fallback scope/evidence](tests/baseline/bc-images.md)
and independent [decoder-core evidence](tests/baseline/bc-decode.md). G07 stays open.

### G08 / P1: SPIR-V transform semantics

Require pre/post spirv-val, reflection diffs, source/binary hashes, and pipeline
specialization keys. Correctly emulate or reject used clip/cull operations;
deleting them and changing the image is not valid compatibility.

**Experimental subset:** fixed scaled probes retain original/transformed hashes,
spirv-val, disassembly, and interface differences. Original modules match build
inputs; four loader paths produce correct pixels. Multi-entry modules get a
temporary module for the requested pipeline stage, pruning unrelated functions
and globals. One three-entry module reused for vertex/fragment/later pipelines,
including helper-function push-constant reads, passes on Adreno/Mali.

Literal operands such as shuffle indices are no longer misclassified as IDs.
A legal shader using ID=3/index=3 has old/new Adreno controls; complex operand
layouts remain conservatively scanned. A separate pass splits aggregate inputs
into per-Location inputs and reconstructs original float Private values.
Dynamic-index/whole-value argument cases pass four shapes; original module
hashes, per-column interfaces, and spirv-val are audited.

Direct/expression array lengths resolve from pipeline specialization parameters.
Defaults, mixed float/bool parameters, repeated module creation, and cache
save/rebuild pass on Adreno/Mali; original specialization bytes/mappings are
associated with transformed modules. Independent decoration-group expansion
passes Location/member-layout/SpecId cases for single/multiple entries and
specialized arrays on both vendor drivers and repaired VVL; SPIRV-Tools
independently checks decoration semantics. Generic interfaces/extensions and
complete specialization/cache keys remain unfinished.

**Automatic clip path:** when physical `shaderClipDistance=false`, the ICD
advertises it, strips its enable bit before HAL CreateDevice, and converts used
ClipDistance to Location varyings plus fragment discard without application
environment variables. Blender 4.3 passes `is_supported` and creates a window
on X300. Composition/presentation remain G11 responsibilities.

### G09 / P1: synchronization and memory semantics

Require non-coherent atom alignment, flush/invalidate, repeated staging writes,
submission reuse, interqueue signals, wait-before-signal, and destruction while
work is in flight. The default implementation must not depend on global wait-idle.

**Partially verified:** two same-family Mali queues submit the consumer first,
then signal from the producer. Four fill→copy→1024-word exact-readback rounds,
timeline/fences, and reuse pass through native/frontend/ICD core/KHR six paths.
Actual non-coherent allocations receive whole-allocation invalidation; both
standard-ICD aliases report zero SyncVal errors. The Adreno vendor driver lacks
the required timeline support. Independent upstream Turnip on Redmi exposes
one queue, so that control is unsupported.

Vulkan 1.0 partial upload/readback probes pass on Mali native/frontend/ICD plus
standard validation: nonzero binding offsets, writes crossing atom boundaries,
finite partial flush/invalidate, unchanged adjacent data, and four command-buffer
resubmissions with zero SyncVal errors. Adreno lacks an applicable non-coherent
memory type on all four paths. Additional tests cover nonzero partial mappings,
VK_WHOLE_SIZE to mapping end, and finite/WHOLE_SIZE ranges ending in an incomplete
allocation-tail atom. Mali passes seven exact-readback rounds on four paths with
zero SyncVal errors; Adreno remains unsupported. Cross-family ownership,
concurrent host submissions, and retirement with other work in flight remain
uncovered. See [memory/synchronization evidence](tests/baseline/memory-sync.md).

### G10 / P1: closed

Product desktop GL is Mesa/Zink, not hybris GL-to-GLES or Gladio. Track Zink
capability gaps under G07/G08. Blender through Zink is not an acceptance condition.
Do not build another GL frontend or add a separate VVL/capture integration for teapot.

### G11 / P1: product windows and WSI lifetimes

Product windows use ICD/Turnip WSI → TAWC-DRI / android_wlegl → anlabwc. The
hybris EGL X11 plugin was removed, and `ws_init` rejects wayland/x11; it is not
a current bypass. **Partial:** evidence covers Xlib/XCB/Wayland creation,
FIFO release, resize/out-of-date, surface-lost after native X destruction, and
product teapot/scene Host gates. Remaining cases include multiple windows,
minimize, disconnect, delayed release, concurrent destruction, long-term FDs,
and real advertised-usage / alpha≠1 operations. Existing create/render/resize/
retirement/destruction and application pixel gates remain valid. Remaining
lifetime/race cases need separate evidence, not restored EGL plugins or a
second present path.

### G12 / P1: reproducible rendering-failure evidence

For a selected frame/draw, retain input shaders, descriptors/resources,
before/after attachments, synchronization events, and presentation records.
Bound storage and keep expensive capture disabled by default.

### G13 / P2: multivendor regressions and CTS metrics

Keep versioned Adreno/Mali baselines with separate unsupported/fail/crash/timeout
counts. Every workaround needs original failure, corrected pass, and other-device
regression evidence. Shader-audit errors are recorded separately and original
probe exit codes retained. An old VVL hang is TIMEOUT 142; CTS coverage has not
been expanded.

BC upload emulation is not full Vulkan BC support: account for application
tiling/usage, copy rules, compatible views, memory requirements, and aliases.
Limit queries/creation for a supported subset rather than setting
`textureCompressionBC` globally true. Removing unused declarations differs from
emulating actual clip/cull operations; output-structure/AccessChain rewrites
must preserve validity and semantics. Timeline and dynamic-rendering emulation
have substantial semantics. Prefer passthrough and investigate missing features
per workload; a few wrappers do not upgrade an entire Vulkan version.

### 5.1 G06 evidence log

On 2026-09-09, actual Blender startup on Redmi product Turnip used standard VVL,
GFXReconstruct, and the shared rendering inspector. Full-size startup waited
on a fence after acquire returned OUT_OF_DATE. An 800×600 control crashed after
dynamic-rendering command merging. Its capture ended before submit returned.
The analyzer now reports live recordings without captured submissions separately:
13 interrupted suspend/resume chains and 81 unpaired resumes. They are not
treated as submitted execution and produce no corresponding draw-resource
requests. X300 submitted-batch reports and resource-request regressions remain
consistent. Both Redmi failures were unresolved, and the original OnePlus/
Adreno 830 failure was not retested. See [device diagnosis and reuse boundaries](docs/blender-turnip-redmi.md).

Rendering-segment conversion was subsequently separated from HAL dispatch and
shared between standalone `VK_LAYER_HYBRIS_compat` and the original ICD. An
isolated Redmi run passed the old command-merge crash: 389 downstream captured
rendering begins were converted, then submit/wait/present succeeded. White
blocks and missing UI remained; complete application status was FAIL. Evidence
covers independent pixels/SyncVal, allocation-failure cleanup, two-device
reconstruction, and X300 original-path regressions. Standard-loader KHR ELF
exports are unavailable and explicitly unsupported. See [shared-layer build,
boundaries, and regressions](docs/vulkan-compat-layer.md). These records do not
close G02/G03/G08/G09/G11 or the complete application gate.

Zero atlas results from actual font draws identified Turnip descriptor-pool
reset errors in VMA heap length and allocation direction. Mesa was fixed. The
shared UBO probe adds ordinary/reset-before-first-allocation modes: original
driver controls fail both; corrected ordinary/SyncVal each pass eight exact-pixel
rounds. X300 native/frontend/ICD regressions pass. Nine-frame replay restores UI;
the real small window completed editing, saving, reopening, OBJ export, and
coordinate checks. Physical-window transparency and default large-window startup
still failed. Full application status stays FAIL. See [descriptor-pool root cause,
fix, and acceptance boundaries](docs/blender-turnip-descriptor-pools.md).

The OPAQUE composition contract was then fixed: TAWC-DRI 0.4 explicitly carries
opacity while preserving translucent behavior for old requests; Mesa's direct
Wayland path sets an opaque region. Redmi passes six-stage scenes, alpha=0/1
raw-pixel/physical-screenshot checks at three Vulkan sizes, zero SyncVal errors,
and product GLX/Wayland teapot regressions. The compatibility layer, driver, and
launcher were deployed to the Redmi product. Reopening the model no longer
revealed the terminal behind it. A new default-size capture still ends with
acquire OUT_OF_DATE; full application status remains FAIL. The new hybris ICD
alpha=0 gate had not been rerun; older alpha=1 results cannot establish a pass.
See [composition fix and product evidence](docs/blender-opaque-wsi.md).

The following evidence uses fixed offscreen fixtures, not arbitrary applications:

- **Layout and pixels:** 272B std140, 12 vertices / 18 indices, correct binding
  `255,255,0,255` and alternate binding `0,255,255,0`, with explicit
  color→transfer→host synchronization. Dynamic UBOs use nonzero descriptor base
  and dynamic offsets. An actual 1232B synthetic shader block checks 14 asymmetric
  mat4 values (array stride=64, column stride=16), trailing vec4/vec2, and
  bool/int fields. Ordinary/dynamic data yields exact pixels on native/frontend/
  ICD; ICD VVL/SyncVal has zero errors. This is not Blender's complete instanced
  widget layout.
- **Upload and reuse:** both UBO sizes use staging copies into unmapped
  device-local memory and reuse descriptors/resources. After fence completion,
  correct/alternate/correct data is rerecorded and resubmitted for six exact
  readbacks per size. Vulkan 1.1 core templates update the same set with a
  nonzero payload offset and a valid but opposite descriptor in the prefix.
  Updates, rerecording/resubmission, pixels, and zero ICD VVL/SyncVal errors pass.
- **Captured resources:** GFXReconstruct exports draw-time descriptors, complete
  272B UBOs, and attachment before/after images. Audits correlate update/bind with
  set/buffer/range, attachments, copy-source images, and subresources. Ordinary
  negative controls diverge first at draw 60 on both devices. Dynamic single
  bindings correlate base+dynamic offset and vertex/fragment dumps, first
  diverging at draw 61. Uncaptured/captured/replayed complete images match.
  Tampered offsets and base-only evidence are rejected. Image/view/framebuffer
  creation, render-pass attachments, copies, and ownership within a submitted
  command buffer are checked; tampered framebuffer/view/draw ownership is rejected.
- **Shader correlation:** ordinary/dynamic captures export API-input SPIR-V,
  matching probe-manifest-verified build snapshots byte-for-byte. spirv-val,
  disassembly, and interface decorations are retained. pipeline.json links
  created/bound pipelines, layouts, set allocations, render passes, shader
  modules, entry points, binary hashes, and paths. Replaced layout/module
  evidence is rejected. pipelineCache=0 is not a driver-internal cache key.

Two sets and four dynamic UBOs, including a two-element array, now have separate
shader-field and full-image checks. Capture reconstructs set/binding/array
ordering, base+dynamic offsets, buffer/memory creation/binding, and complete
two-stage bytes. The negative control identifies set 0 / binding 3 / element 1.
See [multiple-descriptor evidence](tests/baseline/widget-multi.md).

Remaining gaps include nonzero firstSet/partial rebinding, template arrays/
multiple entries/KHR aliases, multiple queues/parallel submissions, generic
descriptor/pipeline history reconstruction, driver-transformed shaders, and
cache keys. Capture-local IDs are not runtime generations. Arbitrary
applications' first incorrect draw and WSI attachment lineage remain unresolved.

### 5.2 Actual Blender capture diagnosis (X300, 2026-09-09)

Standard GFXReconstruct captured two Blender 4.3.2 frames. A checker using
standard JSONL and a pinned Vulkan registry found 17 suspend/resume groups with
intervening action/synchronization commands, plus 96 resumes lacking a known
suspension in their submission. It retains submission, command-buffer generation,
attachment, and draw-binding relationships and generates standard resource-dump
requests. The fork adds index dumps for indexed draws without vertex attributes;
actual widgets yielded 36B indices and a 272B UBO matching captured UpdateBuffer
bytes. See [rendering-capture diagnosis](docs/rendering-capture-analysis.md).

A tool limitation was also isolated: unassisted capture places non-coherent
upload FillMemory after the original flush, so default replay reads zero indices.
Rebind controls and a new standard page_guard capture both read correct indices.
Do not attribute the old replay's zeros to the application or compatibility
layer. See [capture-memory visibility](docs/capture-memory-visibility.md).
Application and replay still have rendering errors. Temporary rendering-flag/
barrier controls are not product compatibility fixes; arbitrary-application
first-bad-draw, full resource history, and WSI lineage remain open.

### 5.3 Blender Vulkan uploads and segmented rendering (2026-09-09)

An indirect-argument fix was verified on X300: real capture showed host writes
to pure INDIRECT_BUFFER resources lacked flushes, now covered by narrowly scoped
upload compatibility. After deployment, move/rotate/scale/extrude/orbit and
screwdriver-model creation, edit mode, save, and reopen passed without an
explicit GPU group error. Native/frontend/ICD/validation memory-range regressions
passed. FAIL records below predate that fix and remain controls. This short
workflow does not close full application, long-term stability, or multidevice
acceptance; subsequent evidence is appended to the same diagnosis document.

Material preview and EEVEE image export ran, but the first export had an unusually
large shadow-buffer count. Three subsequent same/new-process renders did not
reproduce it. Capture found no invalidate call for GPU readback staging; causality
with the shadow anomaly remained unproven, so rendering acceptance stayed open.

Subsequent readback compatibility invalidates the specific GPU-written ranges
after completed submissions. An old-runtime independent probe had 1020 incorrect
bytes in its first round, reduced to zero by explicit invalidate. The new runtime
passed seven readbacks, preservation of CPU-dirty data in shared allocations,
validation, and an independent build containing only those changes. Updated
product libraries completed material preview/orbit and 960×640, 64-sample EEVEE
export. Native/frontend cases omitting invalidate are negative controls, not
driver defects. Redmi lacks an applicable non-coherent type and stays UNSUPPORTED.
Cross-queue behavior, concurrency, every alias, and long-term rendering remain
unverified; see subsequent records in the same document.

Actual X300 diagnosis also found mixed old common/ICD libraries, clip-rewrite
Phi predecessors and unwritten varyings, invalid suspend/resume sequences, and
missing non-coherent flushes for texture staging and immediate vertices.
`tools/inspect-upload-capture.py` derives buffer→allocation→write snapshot→flush→submit
evidence from real page-guard JSONL. It found 77 texture-staging coverage gaps
and one immediate buffer; ordinary buffer-copy staging had no equivalent
candidates. Snapshot time is distinguished from CPU write time; the tool is not
a complete validator.

Compatibility is constrained by the observed Blender API signature and extension
set, preserving real memory properties. It repairs upload visibility and converts
segmented rendering into ordinary rendering that preserves contents. The default
cube, selection outline, splash, text, and toolbar became visible. **Overall
acceptance still FAILS:** three move/rotate updates in the default window caused
tiler heap OOM and GPU timeout; 800x600 multiframe runs also timed out. Mali's
independent non-coherent probes and validation regressions passed; Redmi remains
UNSUPPORTED for that memory type. Multidevice coverage, every alias, complete
secondary/resolve behavior, readback invalidation, and remaining GPU failures were
not accepted. Do not close G03/G08/G09 or application gates. See [diagnosis,
fix scope, and evidence](docs/blender-vulkan-compat.md).

## 6. Diagnosing black screens, corrupt textures, and device loss

### 6.1 Three observation boundaries

```text
A: Application API inputs and advertised capabilities
       | compatibility transform
B: Actual backend calls, shaders, resources, and synchronization
       | GPU execution
C: Attachment -> final image -> WSI buffer -> compositor reception/display
```

Assign stable object IDs plus generations, and separate generations to each
command-buffer recording. Correlate threads/contexts, submissions, draws, frames,
and shader hashes. Reconstruct descriptors when used: ordinary sets, copies/
update templates, dynamic offsets, push descriptors, and, as supported, future
update-after-bind/descriptor buffers. Pointer logs or UpdateDescriptorSets
arguments are insufficient.

CPU writes to mapped memory may have no API call. Capture debug snapshots at
properly synchronized submission/use boundaries and retain flush/dirty ranges.
Host coherence does not allow unsynchronized CPU/GPU access.

Recommended on-demand evidence package:

- `manifest.json`: source/build-id, GPU/driver, native/effective capabilities,
  quirk ID, input fixture/seed.
- `events.jsonl`: a bounded ring of recent N draw/submit states, persisted on
  errors, a selected frame, or explicit request.
- `shaders/`: original/transformed source/SPIR-V, compiler logs, reflection,
  specialization, and pipeline cache keys.
- `resources/`: selected buffer ranges and texture mip/layer/aspect, format,
  stride, swizzle, and colorspace; limit large captures to requested ranges.
- `images/`: before/after selected passes and final submission; exact integer
  comparisons, explicit floating-point/filter tolerances, and separate sRGB/alpha checks.
- `present.jsonl`: buffer IDs, acquire/render-complete/release fence state,
  dimensions/generations, and surface/frame IDs. A received-FD acknowledgment
  does not mean the GPU has finished reading.
- Validation/device-loss records. Query device fault where supported; otherwise
  retain recent CPU-side submissions and the resource graph.

Diagnosis order: no draw → capability choice/dispatch; wrong draw output → B
and its attachment; correct attachment but wrong final image → copy/blit/
composition; correct final image but wrong window → WSI/size/color/fences.
Black may be valid content; use expected results, not color heuristics.
Diagnostic readback/barriers/waits alter timing. Record enabled instrumentation
and retain a disabled control. Defaults must not log every draw or force global
synchronization.

### 6.2 Existing tool integration

| Tool | Purpose | Remaining project validation |
|---|---|---|
| [Vulkan Validation Layers](https://github.com/KhronosGroup/Vulkan-ValidationLayers) | API/object/parameter checks; [SyncVal](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/syncval_usage.md) checks synchronization hazards | Layer chain, boundaries, pinned dependencies; no errors does not prove correct pixels |
| [GPU-assisted validation](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/gpu_validation.md) | Shader-access and descriptor diagnostics | Features/resources may be insufficient; shader/execution perturbation; older devices may not support it |
| [GFXReconstruct](https://github.com/LunarG/gfxreconstruct) | Vulkan capture/replay, SPIR-V extraction, API event conversion | Test without WSI first, then Android/custom WSI; cross-platform/driver replay is not generally guaranteed, nor automatic minimal C reproduction |
| [RenderDoc](https://github.com/baldurk/renderdoc) | Per-frame resources, pipelines, textures, draws | Mixed glibc ARM64/bionic-driver processes are unverified; Android support does not establish attach support here; legacy GL is not fully covered |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Validation/disassembly/optimization/reduction | Pin target environment and transform boundaries; valid SPIR-V does not prove correct driver execution or equivalent semantics |
| [VK_EXT_device_fault](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html) | Fault data after device loss where supported | Not exposed on 29854870; optional, with no promise of recovering a lost device |

If RenderDoc/full replay integration is slow, first deliver repository-local,
bounded per-draw state/image dumps. Do not begin by creating another generic
capture format and complete replay engine.

## 7. Standalone tests and measurement

Pin official [VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) externally and
run Vulkan/OpenGL/GLES/EGL groups incrementally. Routine investigation stays
lightweight rather than downloading/running the entire suite by default. CTS
results differ from a formal Khronos conformance submission. Use [Vulkan
Profiles](https://github.com/KhronosGroup/Vulkan-Profiles) and Zink requirements
to report declared-capability gaps, then verify semantics through actual tests.

| Suite | Proposed coverage | Dependencies |
|---|---|---|
| smoke | Existing baseline plus loading/dispatch checks | adb, glibc runtime, hybris; no windows |
| semantic | Texture/mips/sRGB/swizzle, UBO/descriptors, multipass, synchronization, shader transforms | Same; no windows; explicit references |
| blender-shaped | Mali widget data/draws and Turnip View3D multiresource reuse | Ordinary Vulkan/GLES programs; an unreproduced probe is not a passing regression for the original bug |
| wsi | Buffer handoff, fences/release, resize, color, window lifetimes | Small standalone bionic receiver/compositor and matching Xwayland for X11; no complete Arlinux application required |
| cts | Batched API/version/feature case lists | Pinned CTS/runner; glibc and bionic builds |
| apps | Blender fixtures, default scene/UI, orbit/resize, other target apps | Final integration; record application/version/backend separately |

Keep at least native-vendor, hybris-passthrough, and hybris-compat controls on
the same device and comparable workloads. If an emulated feature cannot run
natively, compare against CPU/reference or another verified implementation;
native NotSupported is not a failure. Retain Mesa/software references without
assuming they can replay every capture. Record CPU submission time, supported
GPU timing, frame latency, and peak memory. Evaluate image correctness and cost
separately.

Counts must distinguish tested/pass/fail/unsupported/crash/timeout with pinned
cases and device versions. Do not collapse different APIs/profiles into a
misleading compatibility percentage. Each application bug needs the original
failing image, target reference, and first incorrect pass, not just startup status.

Baseline additions as of 2026-09-06:

- `tools/build-aarch64.sh` cross-compiles from this repository and writes
  `manifest.json`; `build.sh` builds probes only.
- Builds use clean source/header snapshots and install/runtime staging, retaining
  content hashes, toolchain identity, and deployed ELF inventories. The runner
  validates library/probe manifests and captures actual mappings. Post-run
  Android-path hashes are distinguished from memory-page identity.
- Record the probe PID before exec. After timeout, verify the executable path
  before cleanup. `alarm(25)` runs in the probe constructor and does not cover
  earlier dependency constructors, so a host timeout remains necessary.
- Device directories use `/data/local/tmp/libhybris-baseline-<run-id>`.

Remaining gaps:

- EGL_CLIENT_APIS lacking desktop GL is distinguished from failed EGL calls;
  fuller context/profile feature tests are still needed.
- Shared rwlock destruction and mutex timed locking translate tagged handles
  before calling glibc. Without device `/dev/shm`, successfully allocated shared
  paths remain untested, so shared-synchronization gaps stay open.
- stdio fgetpos/fsetpos use ftello/fseeko for bionic offset semantics, avoiding
  uninitialized glibc fpos reads after failure. Probes cover ordinary/64-bit
  byte-stream positions and pipe ESPIPE, not 32-bit overflow or multibyte state.
- fflush no longer prefilters NULL/memory streams with fileno. Independent
  native controls cover global-file flush and open_memstream publication;
  analogous unlocked-entry corrections lack independent device tests.
- Rwlock-kind bionic/glibc enum translation has native controls; fairness and
  starvation under heavy contention remain unverified.
- Native/hybris controls cover destruction and explicit reinitialization of
  unused private static mutexes/conditions/rwlocks, not shared objects,
  destruction while in use, or repeated destruction.
- ES3-specific features, Vulkan texture/compute, concurrent shared GLES
  resources, and deletion while referenced remain untested. Widget shaders and
  two-thread device/fence tests do not establish API feature-level support.
- Standard validation/capture connects to standard-loader headless widgets
  and windows (G04 Partial). Arbitrary applications remain uncovered. The
  handwritten layer chain bypassing the standard loader was removed; its old
  native successes are not completion evidence.
- G03/G05/G06 still have headless probes only. The optional ICD has instance/
  device generations, not resource generations or complete application evidence
  packages. `unload` exits because hooks are pinned, not because Android Vulkan
  objects can be reclaimed. `init`/`tls` check complete concurrent first
  `android_dlopen` results and worker exit after frontend closure. `tls-dtor`
  observes C++ destructors for both TLS models; `tls-bounds` checks cross-thread
  registration initialization and preservation of local writes. None proves
  unmapping, static-slot reclamation, or every vendor TLS destructor.

## 8. Implementation order and completion gates

1. **Trusted baseline (G01–G05):** standalone build/run, artifact manifests,
   unified dispatch, loader/layer spike. Require complete legal small workloads,
   diagnosed intentional errors, and explained native/passthrough differences.
2. **Diagnostics first (G06/G12):** Mali-shaped UBO/readback/draw state,
   attachment lineage, shader dumps. Inject incorrect bindings/color transforms
   and locate the first failing stage rather than just producing a white screen.
3. **Evidence-driven compatibility (G07–G09):** common BC subsets, scaled
   vertices, known shader/driver quirks, precise capability gates, individual
   switches, and references. Run corresponding Vortek-reference small tests
   before porting algorithms.
4. **WSI (G11):** G10 is closed because desktop GL uses Zink. Submission is
   unified through standard loader → ICD/Turnip → TAWC-DRI / android_wlegl.
   Remaining G11 work is multiwindow/lifetime races, not a second present path.
   Offscreen desktop-gl does not replace application-window gates.
5. **Real application/device regressions (G13):** reproduce and resolve both
   Blender cases on their original devices. Keep original Turnip-bug resolution
   separate from hybris compatibility. Adreno 650 passes do not replace
   Mali/Adreno 830 results.

Each workaround needs trigger conditions (vendor/device/driver/feature),
rationale, transform semantics, enabled/disabled results, regression cases,
cost, and removal conditions. Never silently discard original shaders,
capabilities, or pNext data. Do not promise full OpenGL 4.x/Vulkan 1.3+, every
application, automatic root causes for every GPU black screen, or libhybris-only
repairs to Turnip kernel/compiler defects. Establish a diagnosable, verifiable
compatibility platform, then expand support individually.

Daily experiments/run IDs belong in [implementation status](IMPLEMENTATION.md)
and the relevant `tests/` documents, rather than being appended to this checklist.

### Original Debian Blender after desktop resize (2026-09-09)

The original Debian Blender 4.3.2 package now completed the Mali Vulkan model,
save, fullscreen/restore and reopen workflow in Arlinux's desktop session. The
fix keeps live resized X11 buffers presentable with SUBOPTIMAL and real acquire
synchronization; it does not alter Blender or skip fence waits. An independent
resize run passed exact frames/screenshots, release ordering and validation.
Process identity/debugger tooling exposed additional gaps during diagnosis.
See [failure chain, implementation and limits](docs/native-blender-resize.md).
The broader lifecycle, diagnostics and application gates remain open.
