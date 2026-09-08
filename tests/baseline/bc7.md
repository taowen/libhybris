# BC7 implementation and evidence (2026-09-08)

The default-off BC fallback now includes BC7 UNORM and sRGB. It decodes on
GPU into native RGBA8 images and uses the existing image/view/copy and command
state preservation paths. BC6H was subsequently added; see [BC6H evidence](bc6h.md). `textureCompressionBC`
remains false, and G07 remains open. This is not complete Vulkan BC conformance.

## Independent reference and source provenance

The production GLSL decoder adapts the BC7 algorithm and partition data from
[iOrange/bcdec](https://github.com/iOrange/bcdec) revision
`80859ed3b7afb1c527a2a99d70c61457bea72d0c`, under its MIT alternative.
`hybris/vulkan/compat/shaders/LICENSE.bcdec` contains the notice. BC7 code is
kept in `bc7.glsl`, separate from the BC1–BC5 shader. It handles all eight
modes, subset partitioning, anchor indices, endpoint P-bits, separate alpha
indices, index selection and component rotation. Reserved mode blocks are
not part of the supported fixture evidence.

The golden data comes from a different implementation:
[richgel999/bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) revision
`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`. The unmodified reference files,
MIT notice and SHA-256 provenance are stored under `reference/bc7/`.
`generate.py` compiles the reference in scalar and default SIMD configurations
and requires byte-identical results before writing `bc7_fixture.inc`.
Neither CPU decoder is linked into the ICD or probe executable.

The seeded corpus has 512 blocks (64 per mode), covering every legal partition
number, all P-bit combinations, all mode-4 rotation/index-selection pairs and
all mode-5 rotations. Endpoints and indices vary deterministically; this does
not exhaust all possible endpoints or index patterns. The 128×64 kernel shape
checks every texel of all 512 blocks, with three changed-input submissions of
the same recorded command buffer. The fourth submission uses GPU-filled valid
mode-6 data, with separately generated reference pixels. Prefix/suffix guards,
padded rows/layers, odd extents and split dispatches retain their existing checks.

## Build and device results

A clean `tools/build-aarch64.sh --clean` build completed, staging 17 runtime
ELFs and checking 58 installed ELF paths. The existing probe bundle built with
NDK `27.3.13750724`; the generated kernel passed Vulkan 1.0 SPIR-V validation.
No new unit-test harness was introduced.

Initial kernel runs `20260908T123756-1e4d9555` (Redmi) and
`20260908T123756-02e0f373` (Mali) each exercised native, frontend, ICD and ICD
validation routes: 264 readbacks per route, 2,112 total, zero mismatches and
zero validation errors. Those runs establish kernel behavior only.

After image integration, force-mode runs `20260908T124709-7aa9da54` (Redmi)
and `20260908T124711-5324a919` (Mali) exercised GIPA, GDPA, dlsym and linked
image entry routes under validation. Each route performed 336 image readbacks
across fourteen formats, two shapes, four mip levels and three submissions.
Mali passed all four routes. Redmi failed all four routes; these failures are
retained, not relabeled unsupported. Internal decoder and multi-UBO rendering
passed on both devices. Scaled-vertex emulation was off for these runs: Redmi
was unsupported and Mali passed its native route, so this is not evidence for
Redmi scaled emulation.

## Remaining Redmi reference failure

The initial Redmi image run `20260908T124501-019a347c` reported 357 mismatching
words: 306 from the existing BC1 RGB border issue and 51 in BC7 sRGB.
BC7 UNORM passed all 24 readbacks. Mali's initial image run
`20260908T124502-53b606fd` passed all 336 readbacks, including BC7 sRGB,
copy2, synchronization2 and maintenance4 paths available on that device.

Diagnostic run `20260908T124628-ab3c8d38` identifies printed BC7 sRGB failures
as native-golden-filtered comparisons. The probe compares the emulated image
against a native RGBA8 reference, then separately checks native filtering
against the CPU four-texel average. The latter converts sRGB before filtering
and preserves alpha; it checks swizzles, clamp-to-edge, white, transparent-black
and opaque-black borders. Its thresholds have not been loosened to pass Redmi.
The cause and conformance significance of the native-reference discrepancy
remain unresolved. This evidence does not establish full BC7 sRGB filtering
precision on Redmi, even when emulated/native samples agree.

BC1 RGB borders on Redmi, mutable/alias/external/sparse resources,
cross-queue ownership, maximum resource sizes, performance and CTS remain
outside the established coverage. The fallback stays opt-in with restricted
queries and creation. Historical BC1–BC5 evidence in the related documents
retains the original format counts and results.

The final failure counters in missing-mode run `20260908T124831-14320f1d`
confirm all 51 BC7 sRGB mismatches are native-golden-filtered, with zero
BC/native, nearest-golden, raw-block or sentinel differences. BC1 RGB UNORM
and sRGB each have 153 BC/native differences. No tolerance was changed.
Mali missing-mode run `20260908T124832-b2ec1a1a` passed all 336 image readbacks.
Both runs enabled scaled fallback in missing mode and passed scaled-vertex and
multi-UBO validation; Redmi exercised conversion, Mali used native formats.

Default-off runs `20260908T124943-8d9d9ceb` (Redmi) and
`20260908T124944-7682e406` (Mali), using the final probe bundle and rebuilt ICD,
passed all four decoder routes (2,112 readbacks total), caps and caps2.
Native/frontend/ICD image cases each returned UNSUPPORTED after querying the
fourteen implemented formats and recording the two unimplemented BC6H formats.
Those image controls are not pixel coverage.

## Filter input diagnosis

The probe now logs the four post-border/post-swizzle encoded texels, pixel
coordinate, native packed result and integer sums for the first three failing
BC7 filtered-reference words in each readback. This does not change either
comparison or its threshold. It makes the CPU reference independently
recomputable without a GPU capture or a CPU BC decoder.

Redmi run `20260908T125407-2abbc61e` still reports 357 mismatching words and no
validation errors. For example, one native filtered result is `ffa23b15` for
inputs `ffd08547,ffce6b08,ffd3a089,ffcf7827` (packed AABBGGRR). Recomputing the
sRGB EOTF in double precision yields ideal RGBA byte-scale averages
`21.41267,58.70937,160.86227,255`; the native blue result is 162, an error of
about 1.13773. Thus replacing the eight-bit CPU EOTF table with a more precise
one alone would not explain away all observed differences. Alpha half-way
rounding differences in diagnostic output are within the existing alpha
bound and are not the reason these words fail.

The [Vulkan texel decode specification](https://docs.vulkan.org/spec/latest/chapters/images.html#images-texel-decode)
requires sRGB conversion of RGB before sampling operations. The
[CTS texture filtering reference](https://github.com/KhronosGroup/VK-GL-CTS/blob/main/external/vulkancts/modules/vulkan/texture/vktTextureFilteringTests.cpp)
uses explicit coordinate and color precision bounds (its 2D test reduces
RGBA8 color precision by two bits for non-nearest filtering). That is useful
context, not evidence that this probe passed CTS or that its failure is a
Vulkan violation. This diagnostic batch does not import those broader bounds
or change the existing acceptance criterion.

The example can be recomputed independently with Python:

```python
texels = [0xffd08547, 0xffce6b08, 0xffd3a089, 0xffcf7827]
def linear_byte(v):
    u = v / 255.0
    return 255.0 * (u / 12.92 if u <= 0.04045 else ((u + 0.055) / 1.055) ** 2.4)
print([sum(linear_byte((t >> (8*c)) & 255) if c < 3 else t >> 24
           for t in texels) / 4 for c in range(4)])
```

The diagnostic probe bundle built with NDK `27.3.13750724`. Mali control run
`20260908T125449-9108fdc2` passed all 336 readbacks with zero validation errors.
Production library code and the generated decoder are unchanged in this batch.
