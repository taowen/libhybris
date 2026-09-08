# Multiple dynamic UBOs: draw and resource evidence

The `ubo-multi` fixture covers four dynamic uniform-buffer descriptors across
two sets, including a two-element descriptor array. It extends the existing
ordinary/dynamic widget capture without claiming general descriptor-history
reconstruction or runtime object generations.

## Inputs and expected behavior

All four descriptors use distinct 272-byte std140 widget records in one buffer.
Both vertex and fragment shaders check distinct tags 101–104. The vertex shader
also reads all four MVPs; the fragment shader encodes the selected array element's
parameters, checker and integer fields. Every one of the 256 RGBA8 pixels is
checked: good is `(255,255,0,255)`, alternate is `(0,255,255,0)`. Poison tags in
unselected records produce a different failure color while retaining identity
MVPs, so offset mistakes do not silently remove the geometry.

Dynamic offsets are consumed in set, binding-number, then array-element order,
as specified by [vkCmdBindDescriptorSets](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html).
The first set deliberately declares bindings in reverse order, `[3, 0]`, while
the second set uses binding 1. This distinguishes set ordering from a global
binding-number sort. Four different dynamic offsets and different base offsets
also distinguish array-element order from the descriptor-write order.

All offsets below are multiples of `stride`, the 272-byte record size rounded
up to the queried `minUniformBufferOffsetAlignment`:

| Set | Binding | Element | Descriptor base | Dynamic good / alternate | Effective good / alternate |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 1 | 2 / 2 | 3 / 3 |
| 0 | 3 | 0 | 2 | 3 / 3 | 5 / 5 |
| 0 | 3 | 1 | 1 | 6 / 5 | 7 / 6 |
| 1 | 1 | 0 | 3 | 8 / 8 | 11 / 11 |

Only the dynamic offset of set 0, binding 3, element 1 changes in the negative
render control. It selects another valid record with the same identifying tag
and different fragment fields. The other three descriptors remain unchanged.

## Capture and diagnostic checks

The normal `--capture-tools` path now runs ordinary, single-dynamic and
multi-dynamic captures. Each renders both inputs independently, captures,
converts and replays them, retaining all command exit codes. The three suites
execute 24 capture-stage commands and compare 18 complete 1024-byte image
artifacts per device: reference, captured and replayed for each input/mode.

`descriptor_evidence.py` reconstructs the four effective ranges from captured
set-layout creation, allocation, descriptor writes and binding offsets. It
checks each descriptor's full 272-byte dump in both shader stages against
independent expected records, including every tag. Buffer creation, memory
allocation/binding, device ownership and bounds are associated with each entry.
These are capture-local object IDs and API indices, not runtime generations.

`draw_evidence.py` checks the clear attachment is all zero, the post-draw image
matches the final readback, and the existing framebuffer/view/image/copy/submit
lineage holds. `shader_evidence.py` associates both modules with the pipeline,
checks exact probe-manifest shader bytes and runs `spirv-val`. The comparison
identifies the changed descriptor and the first differing draw attachment.
Hashes of all six host capture/audit scripts are retained in `device.json`.

Pipeline creation and its shader assets moved from `probe_widget.c` into
`widget_pipeline.h`; the main probe shrank from 758 to 677 lines despite adding
the new variant. Existing ordinary, large, staged, template and dynamic-rendering
variants use the same helper. No unit-test suite or runtime diagnostic framework
was added.

## Reproduction

Build the library and probes using the baseline README. To regenerate the new
embedded shaders, run `python3 tests/baseline/shaders/generate-widget-multi.py`;
it compiles and validates Vulkan 1.0 SPIR-V.

```sh
python3 tests/baseline/run.py --serial SERIAL --icd-hal HAL \
  --vulkan-loader LOADER \
  --validation-layer LAYER --validation-manifest LAYER_JSON \
  --validation-build-manifest LAYER_BUILD_MANIFEST \
  --capture-tools tests/baseline/build/gfxreconstruct/install \
  --case native-ubo-multi --case hybris-ubo-multi \
  --case icd-ubo-multi --case icd-ubo-multi-validation
```

Use the existing explicit `--icd-mali-loader-quirk` on the supported Mali build.
Version discovery and the ordinary ICD widget capture dependency are scheduled
automatically. Validation and capture use separate probe executions.

## Final device evidence (2026-09-08)

One clean library build completed in 49.096 seconds, with 17 runtime ELFs and
58 installed ELF paths checked. Probes used the pinned glibc builder and NDK
27.3.13750724. All 72 compiled Vulkan source/header/include/build-rule inputs
and all 91 probe source assets match the worktree. The four final runs retain
identical library and probe manifests. The ICD binary is byte-identical to the
previous BC1 RGB batch; this batch changes probes, capture auditing and code
organization, not driver compatibility behavior.

| Matrix | Redmi 29854870 | Mali 10AFA31610002QH |
| --- | --- | --- |
| All six widget variants, their validation cases, rendering entry regressions, three capture suites | `20260908T121659-c81e25af`: 28 PASS, 18 UNSUPPORTED | `20260908T121659-7ea849f5`: 43 PASS, 3 UNSUPPORTED |
| BC `force`: version, ordinary/multi-widget validation, BC image validation | `20260908T121721-c881dd08`: 3 PASS, 1 FAIL | `20260908T121723-31b91062`: 4 PASS |

The six widget variants are ordinary, single-dynamic, multi-dynamic, large,
staged and template. Each passes native/frontend/ICD execution and ICD
validation on both devices. Applicable dynamic-rendering core/KHR entry cases
also pass after the pipeline extraction. Redmi lacks their required API or
extensions, so all 18 rendering cases remain UNSUPPORTED. Mali's three
UNSUPPORTED cases are native KHR dlsym, ICD KHR dlsym and ICD KHR linking:
`vkCmdBeginRenderingKHR` is not exposed through those loader ELF entries.
They are not counted as executed rendering coverage. All executed widget
validation reports have zero errors.

Both devices pass all three capture suites, with every one of the 24 recorded
capture-stage commands exiting zero and all 18 full-image artifacts matching.
The first attachment divergence remains draw 60 for ordinary binding and draw
61 for single-dynamic binding. The new multi case diverges at draw 62, with
only set 0 / binding 3 / element 1 changing its resource contents. The clear
attachments are exactly zero. Both shader stages' four UBO dumps match their
expected 272 bytes, and both modules match the manifest and pass SPIR-V
validation. Redmi stride is 320 bytes; Mali stride is 272 bytes.

On each device's saved multi capture, 11 independent evidence mutations are
rejected: global binding-order offsets, swapped array offsets, omitted dynamic
offsets, duplicate array indices, a missing array dump, swapped set handles,
an incorrect array layout, a missing backing allocation, undersized memory,
wrong buffer device and wrong pipeline layout. Reversing layout declaration
order or descriptor-write order remains accepted. These are 22 offline
rejections and four accepted ordering controls across the two devices, not
additional GPU executions or a unit-test suite. Local audit results and its
reproduction script are retained under `build/widget-multi-audit/`.

The BC-enabled regression also passes ordinary/multi-widget validation on both
devices. Mali passes all 288 BC image readbacks. Redmi retains the same 306
mismatched words in 12 BC1 RGB transparent-border readbacks; the other 276 have
zero mismatches. Both BC image runs report zero validation errors. Redmi's
runner returns failure for this real pixel error.

Final SHA256 values:

- ICD: `865db0908df892339ca8c616ef44fe19d32295668a2df107819dee1becf332ac`
- probe-glibc: `b3a4651500e3f3149a09cfdb91a765b81ac6759d9c1d60e912a4bfe62b421a80`
- probe-glibc-linked: `3b3b7de6d899fab46ba43ff7843cba5a2d3f1e16209f992870c8fb4dc98912e1`
- probe-bionic: `8b6e62f3c50b248242a499ec0fbce6e90517956c4cdec29b273315b9e26f2bf9`

## Remaining scope

This still requires one descriptor allocation/update/bind and one draw per
captured fixture. Templates and staging have separate pixel regressions, but
are not reconstructed by this capture audit. Partial rebinding with nonzero
`firstSet`, descriptor copies, template arrays, push descriptors, update after
bind, descriptor buffers, handle reuse, general pipeline/layout compatibility,
parallel submissions and WSI attachment lineage remain open. The rendered
negative control is deliberate valid input, not a newly discovered vendor bug.

The BC compatibility implementation is unchanged. Redmi's BC1 RGB transparent
border error remains a real FAIL, documented in [bc-rgb8.md](bc-rgb8.md); G06,
G07 and the full gaps checklist are not closed by these results.
