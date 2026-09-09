# Blender Vulkan diagnostics and bounded compatibility

X300 / Mali-G1-Ultra, Blender 4.3.2, 2026-09-09. **Whole-application acceptance
remains open.** The default scene, splash, text and toolbar now render, but
repeated scene updates still produce Mali queue timeout messages. The large
window run also reports a tiler heap OOM. Correct screenshots and successful
Vulkan return codes do not override these failures. Android screen captures
also show a partially transparent viewport background over the terminal,
whereas Blender's own readback is opaque; composition acceptance remains open.

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

Evidence lives in the Ardesk parent workspace under
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
