# Blender Vulkan diagnostics and bounded compatibility

X300 / Mali-G1-Ultra, Blender 4.3.2, 2026-09-09. **Whole-application acceptance
remains open.** The default scene, splash, text and toolbar now render, but
repeated scene updates still produce Mali queue timeout messages. The large
window run also reports a tiler heap OOM. Correct screenshots and successful
Vulkan return codes do not override these failures. The RGBA opaque-presentation
correction below removes the transparent viewport background in Android screen
captures; the remaining GPU faults still prevent application acceptance.

## Separate causes and evidence

1. A product installation mixed the current ICD with an older common library.
   The latter did not automatically activate the known-build Mali MMUD hook.
   Replacing common alone made the original first pipeline compile. The
   isolated runtime was staged as one manifest-verified set. The product
   installation has subsequently been updated as described below.
2. Clip rewriting split a fragment entry block without updating existing
   `OpPhi` predecessor labels. The corrected shader passes `spirv-val`.
   Planning also used an unused ClipDistance declaration that module creation
   had removed, producing an unwritten varying and an undefined fragment kill.
   Planning now applies the same unused-built-in removal first. See
   [the shader investigation](../tests/baseline/clip-distance.md).
3. The captured render graph contains interrupted suspension/resumption chains
   and unmatched resumes. The [rendering inspector](rendering-capture-analysis.md)
   identifies the commands and submissions; the application profile lowers
   these segments into ordinary rendering with explicit content preservation.
4. Two host upload paths omit noncoherent flushes:
   [`VKTexture::update_sub`](https://github.com/blender/blender/blob/v4.3.2/source/blender/gpu/vulkan/vk_texture.cc)
   converts directly into mapped staging memory, and
   [`VKImmediate::end`](https://github.com/blender/blender/blob/v4.3.2/source/blender/gpu/vulkan/vk_immediate.cc)
   records a draw after writing/converting mapped vertices. In contrast,
   `VKBuffer::update_immediately` and the ordinary vertex/index staging paths
   explicitly flush. Blender's bundled VMA requires HOST_CACHED for random
   host access, selecting Mali type 1 (flags `0xb`, noncoherent).

The host-upload observation can be reproduced without relying on a screenshot:

```sh
python3 tools/inspect-upload-capture.py calls.jsonl --output uploads.json
```

On the original page-guard capture, the inspector examines 210 noncoherent
host-written buffers and finds uncovered snapshots in **77 texture staging
buffers and one immediate vertex buffer**, with no buffer-copy staging
candidates. The immediate buffer is capture ID 212, created at call 335,
bound to memory 182 at offset 159744, size 4194304, usage `0x82`; its first
recorded binding is call 1615 and first submission is 3179. Reported flush
ranges never cover its captured writes before those submissions.

These are bounded capture observations, not a validator. FillMemory metadata
records snapshots rather than exact CPU write times. The tool deliberately
reports absent coverage by *any* earlier flush; it can miss a later unflushed
rewrite. It does not infer exact vertex fetch sizes, model all resource
lifetimes, or see flushes inserted below the capture layer. The older
unassisted capture/replay has a separate visibility problem described in
[capture memory visibility](capture-memory-visibility.md).

## Runtime scope

`compat/application_policy.c` matches the observed public application and
engine names (`Blender`), both version fields (1.0.0), Vulkan API 1.2, and the
observed device extension set. Blender does not report its release number
here, so this is **not** a reliable 4.3.2 version check. Unknown extensions
turn the profile off. No executable-path or environment switch is used.

`compat/memory_visibility.c` tracks buffer bindings, allocation generations,
actual property flags and mapping windows. At copy-to-image recording it
flushes only mapped noncoherent buffers with pure TRANSFER_SRC usage. At
vertex binding it handles the observed immediate usage VERTEX_BUFFER |
TRANSFER_DST. These render-graph uploads are host-written before recording
and retained until their GPU consumers retire. Ranges include binding offsets,
are atom-rounded, and must fit the real mapping/allocation bounds. Allocation
callbacks are preserved; driver calls and callbacks run outside metadata locks.
Memory types, requirement masks and advertised properties remain unchanged.

VMA maps whole pools. A GPU-written vertex buffer can therefore share a mapped
allocation with a CPU upload buffer. Flushing every mapped vertex buffer
actually reproduced missing toolbar glyphs; restricting the usage removed
that corruption. The profile does not flush whole allocations at submission,
and does not implement arbitrary coherent-memory emulation, sparse/external
uploads, command reuse after later host writes, or general readback invalidation.
A new application upload pattern needs its own evidence and treatment.

`compat/rendering_segments.c` clears suspension/resumption flags consistently
for the profile. Resumed attachments LOAD; suspended attachments STORE and
suppress resolves until the final segment. Ordinary initial CLEAR behavior and
final resolves remain intact. A GPU memory dependency precedes profile rendering
begins. This does not add CPU queue/device idle waits. Unsupported extension
families such as implicit multisample resolves disable the profile. Full
secondary-inheritance chains, multiview/query interactions and multisample
resolve equivalence have not received independent runtime acceptance.

`icd/commands.c` owns only command/pool metadata, preserving driver handles and
per-device dispatch. Pool callbacks own command metadata and rendering scratch
allocations. Recording allocation/visibility errors propagate at command end.
No general resource graph, queue retirement framework or WSI changes were added.

## Validation and remaining failures

Evidence lives in the Arlinux parent workspace under
`build/mali-pipeline-investigation/20260909T101306/`. Each application result
retains the command, runtime hashes/manifest, loaded maps and screenshots.
These runs include the user's existing uncommitted WSI/fence changes, preserved
by this work; they are integration-worktree evidence, not a clean-checkout
application acceptance claim.

| Evidence | Result |
|---|---|
| `application-final-build.log` | AArch64 build and staged ELF manifest pass |
| `upload-analysis-pageguard.json` | 77 texture + one immediate vertex missing-flush observations |
| `blender-phi-fix-small-124126` | Texture flush alone restores splash/text; toolbar incomplete |
| `blender-scene-phi-fix-small-124953` | Scoped texture + immediate flush: cube, outline, toolbar and text visible; bounded startup/quit |
| `blender-sequence-phi-fix-normal-125337` | Default window, no GPU debug option; three screenshots show cube translation/rotation, but tiler heap OOM and two GPU timeouts: **FAIL** |
| `blender-sequence-phi-fix-small-125618` | 800x600, GPU debug option; three updates, but three GPU timeouts: **FAIL** |

Existing independent baseline probes against the final runtime:

- X300 `20260909T125254-39f4d7af`: ICD version and both memory-range cases PASS;
  the latter include the standard validation/SyncVal route. These are profile-off
  controls and retain actual noncoherent memory semantics.
- Redmi `20260909T125256-31e4f9d0`: ICD version PASS; noncoherent memory-range case
  UNSUPPORTED, consistent with the vendor memory types.

No new unit tests were added. Multi-device/profile dispatch, allocator failure,
all aliases and rendering resolve combinations need further execution coverage.
Repeated readback invalidation is not claimed fixed. The remaining GPU faults
must be localized independently; neither shrinking the window nor accepting
successful return codes closes G08/G09 or the Blender application gate.


## Product deployment and follow-up

`product-deploy-130636` stages the same tested integration runtime into X300's
actual `rootfs/usr/lib/hybris`, patches the normal guest runpaths, retains the
whole prior directory and a local tar backup, and verifies every deployed file
hash. Libraries used by live desktop processes are never overwritten in place.
No APK/compositor restart or WSI source edit was performed.

`blender-product-130707` uses the normal Blender launcher and product library
search path, with no isolated ICD override. Loaded maps confirm the product
ICD/common. Its default-window screenshot shows the splash, scene, text and
toolbar, and it quits normally, but two GPU timeouts keep acceptance **FAIL**.
A subsequent ordinary launch, `build/blender-vulkan/x300-compat-live-1309.log`
in the parent repository, leaves Blender open for interaction; its Android
screenshot records the remaining transparent viewport background.

A resume-only dependency experiment (`blender-sequence-phi-fix-normal-131103`)
still produced a tiler heap OOM; it was not promoted to the product. The tested
rendering dependencies remain unchanged. The clean committed checkout build
at `2cc48f9` also passes (`application-clean-checkout-build.log`).

Dispatch metadata stays active for a matched instance even when an unknown
device extension disables its compatibility operations. This keeps mixed
GIPA/GDPA allocation and recording paths consistent; the per-device flags
still decide whether uploads or rendering are transformed.

## Opaque presentation correction

The transparency has a separate cause: OPAQUE was accepted while native
buffers remained RGBA. The Android compositor consults the imported buffer's
format, so zero alpha in the viewport exposed the terminal underneath.
RGBA OPAQUE swapchains now allocate RGBX backing. Vulkan retains its RGBA
format and alpha storage behavior; the compositor ignores alpha. Actual Mali
native import and rendering succeed with this backing. The Vulkan AHB
[format mapping](https://docs.vulkan.org/spec/latest/chapters/memory.html#memory-external-android-hardware-buffer)
also treats RGBA and RGBX as the same Vulkan format.

Opaque backing is currently implemented for RGBA only. OPAQUE is advertised
only when all offered surface formats are covered (the RGBA-only surface).
BGRA formats remain available with INHERIT; opaque BGRA is not claimed.
Acquire, resize, release and destruction synchronization were not changed.

- `blender-scene-phi-fix-normal-131715`: Android screenshot now shows a fully
  opaque viewport, with the cube and toolbar; one GPU timeout still means
  whole-application FAIL.
- X300 XCB resize `20260909T131946-ea9173cf`: existing independent probe PASS.
- Redmi XCB present `20260909T132059-ce0d8443`: advertises RGBA/BGRA and INHERIT;
  the OPAQUE-only probe correctly reports UNSUPPORTED, not a pixel pass.
- `opaque-final-build.log`: build PASS. `product-deploy-132100`: actual product
  updated with full file-hash verification and prior-directory backup.

The earlier transparency observations remain the pre-fix control. GPU timeout
and tiler heap errors still require diagnosis; this correction does not close
the application or complete WSI acceptance gates.

The ordinary product launch `build/blender-vulkan/x300-opaque-live-1323`
(parent repository) remains open at Quick Setup, with the scene, toolbar and
opaque background visible in its Android screenshot. Its `.maps` file confirms
the product runtime loaded by PID 17223. This launch uses `--debug-gpu` and no
Python script or Blender screenshot/readback operation. Its `.log` nevertheless
records `GROUP_ERROR_TILER_HEAP_OOM` during a submitted workload's fence wait,
followed by a successful wait and further presentation. Thus the diagnostic
screenshot script is not necessary to trigger the heap fault, and subsequent
successful presentation does not establish fault-free operation.

## Overlay isolation and replay fidelity

`blender-overlay-check-*` runs in the same investigation directory use the
installed product libraries, factory startup, no GPU debug option and no
Blender screenshot operation. The timer requests three cube updates; Android
screenshots observe composition. The already-open Quick Setup instance remains
idle throughout. These are bounded diagnostic controls, not feature removals
or application acceptance passes. `overlay-comparison.json` retains the run
names and driver-event log lines.

- Default overlays: one heap OOM and two timeouts.
- All overlays off: two runs complete three updates without a recorded fault.
- Selected outline off: the first run has no recorded fault, but repetition
  reports four heap OOM events. The first result does not isolate the outline.
- Grid off, extras off, and smooth wire off each still produce GPU faults.
- Only grid enabled: no recorded fault in one run. Only selected outline
  enabled: three timeouts. These controls identify a smaller reproducer, not
  the failing draw or the underlying cause.
- Standard SyncVal confirms it is active, reports no corresponding validation
  error, and still reproduces two timeouts.

`blender-overlay-check-product-only-outline-capture-133638` captures frame 3
with standard page-guard tracking. Conversion and the existing rendering
inspector identify submit 6970, command-buffer begin 6629, and three candidate
draws: 6789 writes the R16_UINT outline ID/depth prepass; 6797 detects outlines;
6821 composites antialiasing. The generated `draw-resources.json` requests these
draws with their actual rendering boundaries.

`replay-outline-resources-134109` produces all three resource reports, but has
two queue faults at log lines 3889 and 3895 **during state restoration**, before
the state-loading completion at line 4435. Its indirect prepass parameters are
zero. Neither those values nor the later attachment dumps prove the original
application's shader inputs. The replay also lacks the application's active
compatibility-profile message. Replay extension/feature changes and profile
equivalence must be resolved before interpreting these resources as a draw
failure. The earlier `134036` attempt only failed to locate the product ICD;
it is a launch-configuration failure, not driver evidence.

The shared WSI host now recognizes the driver's explicit `Received a
GROUP_*ERROR_* error on group(...)` messages, records a total and at most 64
log/line references, and forces the final status to FAIL even after successful
exit. The real `134109` replay verifies this path: exit 0, two recorded faults,
final FAIL. This is an observed-message rejection rule, not a complete GPU
health detector; absence of matching messages never establishes acceptance.

### Verified replay profile difference

`replay-outline-devicecapture-134734` captures the replayer's actual API calls.
It preserves Blender's application signature, but adds
`VK_KHR_external_fence_fd`, `VK_KHR_external_semaphore_fd` and
`VK_KHR_depth_stencil_resolve`. These disabled the former strict profile.
They now remain eligible: external synchronization does not alter rendering
segment semantics, and explicit depth/stencil resolve is already core in the
matched API 1.2 and represented by the existing attachment resolve fields.
Other unknown extensions still disable the profile.

After the actual ICD rebuild (`replay-profile-runtime-build.log`), isolated
`replay-outline-profile-135100` reports profile flags 0x3 and completes the same
frame-3 state restoration and three resource dumps without a recorded fault.
The preceding `134909` attempt used an unchanged ICD after rebuilding only the
probe bundle; it is not evidence for the new runtime.

The frame-3 indirect parameters nevertheless remain zero. A fresh capture from
startup (`blender-overlay-check-product-only-outline-capture-135218`, frames
1–3) retains the compute work generating them. Its rebind resource replay
`replay-outline-startup-135648` reads indexCount 36, instanceCount 1,
firstInstance 2 at prepass draw 4860 and nonzero ID, detection and composite
attachments at draws 4860/4868/4892 in submit 5054. Plain replays without
resource dumps, `replay-outline-plain-rebind-135752` and
`replay-outline-plain-default-135816`, also record no fault. These are replay
observations, not proof that the live application's fault is fixed.

The captured replay device also enables timelineSemaphore and sampler YCbCr
conversion; capture/replay alters buffer usages for readback. Those differences,
memory restoration, and submission scheduling remain relevant to fidelity.
Do not infer a shader defect from the earlier zero indirect parameters or
infer application stability from successful replay.

### Direct interactive check

The product session `build/blender-vulkan/x300-interactive-continue.log` was
operated through Android input, without a Blender Python test script. Keyboard
commands moved the cube to X=3 m, rotated Z=45 degrees, scaled all axes to 1.5,
entered Edit Mode and extruded the selected mesh along Z. Screenshots under
`build/blender-vulkan/interactive-evidence/` show the changed transforms and
geometry. During these operations the driver reported three tiler heap OOMs
and three timeouts; grid/selection overlays disappeared and later returned.
Input and editing work, but interactive rendering acceptance remains FAIL.
The application is left open with the unsaved diagnostic scene.

The previous Quick Setup window disappeared after Continue and its old PID
was absent. Its log contains no conclusive exit cause; this observation alone
does not establish a reproducible Continue crash. The fresh launch opened the
ordinary splash and allowed the interactions above.


### Host-written indirect parameters and live modeling (2026-09-09)

The startup capture `blender-overlay-check-product-only-outline-capture-135218`
contains another missing host flush: buffer 1546 (create 4543), usage 0x100,
size 5120, bound to memory 190 at offset 12201984. Memory type 1 has flags
0xb (host visible, cached, non-coherent). Snapshot 4564 writes 4096 bytes at
that offset, consumed by indexed indirect draw 4860 in submit 5054, without
an intervening covering flush. Blender 4.3.2 VKDrawList writes mapped draw
parameters directly. The upload inspector now includes pure indirect buffers;
it reports 76 buffers with uncovered snapshots, including two indirect buffers.
Capture snapshots alone do not establish the exact timing of CPU writes.

The existing Blender-scoped upload workaround now flushes mapped non-coherent
pure INDIRECT_BUFFER allocations before nonempty DrawIndirect and
DrawIndexedIndirect commands. It retains allocation-generation checks,
atom alignment and mapped-range bounds. Compute-produced buffers with additional
usage bits (including the observed 0x123 buffer) remain excluded. This is a
workaround for the observed recording-time host writes, not general support for
writes after recording, indirect-count aliases, GPU-written pure indirect
buffers, or arbitrary applications matching a usage mask.

`indirect-upload-build.log` records an actual incremental ICD build. Default
three-frame application runs `blender-sequence-phi-fix-normal-141726` and the
repeat recorded in `indirect-upload-repeat.log` completed without reported
GPU group errors. These artifacts and the capture analysis reside under
`build/mali-pipeline-investigation/20260909T101306/` in the parent project.
Deployment `product-deploy-141847` verified installed library hashes.

The deployed product session `build/blender-vulkan/x300-indirect-fixed-live.log`
was operated through Android keyboard/pointer input: move X=3, rotate Z=45,
scale 1.5, enter Edit Mode, extrude Z=1 and orbit. Grid, selection and edited
geometry remained visible. The same live Blender then created a screwdriver
through its Python console: 17 mesh objects, 3808 vertices, 2937 faces and six
materials. Edit Mode was checked; the final model was saved and reopened in
the live application. The blend, final screenshot, Edit Mode screenshot and
creation script are in `build/blender-vulkan/screwdriver/`. No explicit GPU
group error was found in this session log through the save/reopen check.
This establishes this modeling workflow, not general Blender stability or
Cycles/material-preview/render-export coverage. The earlier failing session
above remains valid evidence of the pre-fix behavior.

Independent regression `tests/baseline/build/results/20260909T143142-8c1025ea`
passes ICD version and native/frontend/ICD non-coherent memory-range probes,
including the ICD validation case. These probes check existing memory behavior;
they do not independently exercise Blender's indirect-upload policy. The first
attempt `indirect-baseline.log` omitted ICD provisioning arguments and rejected
unknown selected cases; it provides no passing coverage. The corrected run is
`indirect-baseline-corrected.log`. No new unit tests were added. This build also
contains concurrent WSI/fence integration changes; the live results are not a
clean-commit isolation test. Redmi, long-running interaction and the remaining
application gates are still unverified for this change.


### EEVEE preview, image export and unresolved readback (2026-09-09)

The same live product session subsequently switched to Material Preview through
the viewport button. The screwdriver shows environment reflections and distinct
rubber, polymer and metal appearance (`screwdriver/material-preview.png`). A
camera and three area lights were added in memory; EEVEE exported a 960x640 PNG
in approximately 1.93 seconds. The model with camera/lights was saved separately
as `Screwdriver-render-check.blend`, leaving the original model intact. The
creation/render scripts, PNG and elapsed-time report are in the parent
`build/blender-vulkan/screwdriver/` directory.

**Render acceptance remains open:** the first live export logged five
`Shadow buffer full` errors with counts 823775308, 1879824896, 443685896,
215639404 and 1929379841 against capacity 2048. Successful file export and a
recognizable image do not establish correct shadow state. Three subsequent
renders in this same process returned FINISHED without additional such errors;
a fresh process at 960x640 / 64 samples also did not reproduce them. Do not
attribute the first error solely to capture or resolution differences.

A fresh 320x240 / four-sample render was captured with GFXReconstruct page-guard
tracking, without a frame trim. It exited successfully and produced an image;
it did not reproduce the shadow errors. Artifacts under the parent
`build/mali-pipeline-investigation/20260909T101306/`:

- `blender-render-check-product-only-outline-capture-143915`: capture and render.
  The inherited variant name does not describe overlay changes in this runner;
  it opens the saved render scene. `stage/check.py` is the actual workload.
- `replay-blender-render-capture-144010`: **conversion only**, not replay;
  `calls.jsonl` includes binary references.
- `blender-render-check-product-overlay-on-144026`: same small render without
  capture; no observed shadow or GPU group errors.
- `blender-render64-check-product-overlay-on-144123`: fresh 960x640 / 64 samples,
  also without those errors. These remain observations without golden pixels.

The capture contains eight pure TRANSFER_DST buffers, seven of size 32 and one
614400-byte image readback. All bind to mapped memory type 1; allocations 191
and 2750 are mapped at calls 307 and 9414. For example, buffer 2040 is created
at 6116 and bound at 6118 to allocation 191, offset 16248832; copy 6138 writes
32 bytes, submit 6142 signals fence 175 and wait 6143 succeeds. The entire
capture contains **zero vkInvalidateMappedMemoryRanges calls**. The inspected
Blender 4.3.2 `VKStorageBuffer::read` copies into a DeviceToHost staging buffer;
`VKBuffer::read` submits for read and then memcpy's mapped memory, without an
invalidate. This is evidence of a missing readback cache operation, but does
not by itself prove the observed shadow-count error's root cause or identify
CPU read bytes from the capture.

No invalidate workaround has been enabled based on this observation. A safe
implementation must associate GPU-written ranges with completed submissions,
preserve atom boundaries and allocation generations, and avoid invalidating
unrelated dirty host writes in the shared mapped VMA pool. Globally invalidating
all mapped memory after any successful fence wait would not satisfy that gate.
The original live model was reopened after the diagnostic renders.


### Readback visibility correction (2026-09-09, subsequent batch)

The compatibility profile now records GPU writes to persistently mapped,
non-coherent, pure TRANSFER_DST buffers. Buffer copy/fill/update commands retain
atom-aligned allocation ranges and allocation generations; image readback uses
the complete logical staging buffer. Command recording and submission retirement
are separate modules. A successful fence observation invalidates that submission
and earlier writes on the same queue. Unrelated queues are not inferred complete.
Fence polling and application-requested queue/device idle are also handled.
No queue/device idle call is added by the workaround. Physical memory properties,
allocation choices and GPU rendering commands remain unchanged.

Snapshots are independent of command resets; command/pool/device cleanup releases
owned metadata. Submission publication precedes the backend call so a concurrent
successful waiter can see it, and failed submissions remove their snapshots.
Completion processing serializes successful waiters and checks actual fence
status for wait-any. Allocation generations and current mapping bounds are
rechecked before cache maintenance. This is scoped to the observed Blender
profile, not general coherent-memory emulation.

The independent `blender-readback` probe extends the existing memory-range
workload. Source and destination share a mapped non-coherent allocation, with a
separate dirty CPU atom between them. It warms destination CPU cache lines and
intentionally omits the application's invalidate before checking GPU results;
an explicit invalidate afterwards is the reference. Thus native/frontend runs
are **negative controls for the application omission**, not valid-workload
failures of the native Vulkan implementation. Their raw FAIL results are retained.

On the old runtime, result `20260909T151625-53555966` reports 1020 stale bytes in
the first ICD read and 268–778 in later rounds; explicit invalidation gives zero
incorrect bytes in every round. Native and frontend show the same missing-cache
failure. The corrected ICD has zero incorrect bytes both before and after the
explicit reference invalidate, and preserves the dirty CPU gap in all rounds.
The workload covers ordinary fence wait, an unfenced submission followed by an
empty fenced marker, an unrelated zero-timeout wait, fence polling, queue idle,
device idle, wait-any, command reset, pool reset and partial mapping.

- `20260909T152011-6a5783a0`: corrected ICD readback and validation PASS; ordinary
  non-coherent memory validation remains PASS.
- `20260909T152655-45d6c04e`: the same three cases PASS on an independent build
  containing only this batch's source changes, without the concurrent WSI/fence
  worktree edits. `readback-clean-build.log` records the actual clean build.
- `20260909T152011-4c9c02e6`: Redmi ICD version and UBO rendering PASS; native,
  frontend and ICD memory-range tests plus ICD readback are UNSUPPORTED because
  the required host-visible non-coherent memory type is absent.

The first validation run `20260909T151726-97ee818d` had four command/fence reuse
errors after wait-any. The probe now explicitly queries the completed fence
**after** checking readback bytes, so validation can retire its commands before
reuse; the unassisted read remains a wait-any check. That earlier run is not a
zero-validation-error result.

An earlier attempt to restrict these buffers to actual coherent memory was
rejected. Blender's bundled VMA 3.0.1 requires HOST_CACHED for HOST_ACCESS_RANDOM,
whereas Mali's coherent type lacks HOST_CACHED. The resulting allocation failure
led to a Blender resource lookup crash in
`blender-render64-check-phi-fix-normal-145610`. The initial probe modeled a cached
preference and therefore did not represent this allocator constraint. Its passing
results do not validate that rejected approach. No memory-type restriction or
fabricated memory property is retained. See the bundled
[VMA selection code](https://raw.githubusercontent.com/blender/blender/v4.3.2/extern/vulkan_memory_allocator/vk_mem_alloc.h).

With the final invalidate implementation, isolated
`blender-render64-check-phi-fix-normal-151826` completed a 960x640 / 64-sample
EEVEE export, with no observed shadow-counter or GPU group errors.
`product-deploy-152822` then backed up and hash-verified the product library update.
The real product launcher reopened the saved screwdriver, allowed Material
Preview/orbit, and exported the same render in about 1.56 seconds. Evidence is
`build/blender-vulkan/x300-readback-fixed-live.log` and
`build/blender-vulkan/screwdriver/{Screwdriver-readback-fixed.png,readback-fixed-report.json,readback-fixed-render.py}`
in the parent project. The original model was reopened afterwards.

The product render still includes the concurrent WSI integration; the isolated
headless regression above is the clean-source evidence. This batch does not prove
long-running render stability, golden shadow pixels, cross-queue/concurrent-wait
behavior, all copy/submit aliases, secondary-command execution, custom-allocator
fault injection, aliasing or mapping only after recording. Those gates remain
open. It closes the demonstrated non-coherent readback omission for the exercised
paths, not all Blender rendering failures.


The final source additionally disables this readback policy for multi-physical-
device groups. This exclusion was added after the product render above, which
uses one physical device. `readback-final-build.log` and
`readback-clean-final-retry-build.log` record builds of that final source; the
first simultaneous clean-build attempt failed on container `/out` permissions
and is not passing evidence. The serial retry succeeded. The already-running
product session and `product-deploy-152822` retain the preceding build with the
same single-device behavior; multi-device behavior is not claimed from it.

Final clean-source regression `20260909T153626-aa030a81` passes ICD version and
all seven readback rounds with validation (zero errors).
