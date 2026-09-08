# Widget draw capture evidence

Resource, shader and attachment reconstruction for fixed widget draws. [Tool setup](loader-tools.md) and [capture execution](capture-execution.md) describe how to run it. Later multi-set coverage is separate in [widget-multi.md](widget-multi.md).

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

## Dynamic descriptor capture

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

## Shader and pipeline evidence

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

## Attachment lineage

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
