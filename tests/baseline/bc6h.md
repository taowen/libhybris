# BC6H implementation and evidence (2026-09-08)

The default-off BC fallback includes BC6H UFLOAT and SFLOAT GPU decoding and
restricted sampled-image operations. All sixteen BC formats now have a fallback
implementation. `textureCompressionBC` remains false: the resource domain and
remaining failures do not establish full Vulkan BC support. G07 remains open.

## Implementation and independent reference

`compat/shaders/bc6h.glsl` adapts iOrange/bcdec revision
`80859ed3b7afb1c527a2a99d70c61457bea72d0c` under the MIT alternative retained in
`LICENSE.bcdec`. It handles fourteen valid modes, two-subset partitions,
anchors, signed endpoints, transformed deltas, interpolation and binary16
output. Reserved mode prefixes are outside the fixture evidence.

The independent offline oracle is Mesa's `texcompress_bptc_tmp.h`, revision
`c3b008c1ba01d455351b762253ef44c3ca19653f`. Its unmodified source, MIT notice,
adapter, source hash and generated corpus hashes are under `reference/bc6h/`.
The generator requires identical results from two compiler optimization levels.
Neither CPU decoder is linked into the runtime or probe. The corpus contains
952 blocks: 64 seeded configurations and four fixed payloads per valid mode,
including all 32 partitions for each two-subset mode. Both signed and unsigned
outputs use exact half-bit references. This is not exhaustive endpoint/index
coverage.

The image path selects native RGB16 SFLOAT when sampled/transfer capabilities
permit it, otherwise RGBA16 SFLOAT with the existing default-alpha handling.
The tested Mali uses RGB16; Redmi uses RGBA16. Selection uses physical-device
format capabilities, not vendor names. RGB16 staging packs pairs of pixels to
avoid overlapping word stores. Queries and creation retain the existing
sampled/transfer, optimal-tiling, single-sample restrictions. Mutable, sparse,
external, aliasing and cross-queue semantics remain outside this implementation's
established coverage.

## Build and initial device evidence

A clean AArch64 runtime build completed with the integrated image implementation.
The probe bundle built using NDK `27.3.13750724`; generated decoder and sampling
SPIR-V passed their validators. No unit-test framework was added.

Image runs `20260908T134333-ffad6907` (Redmi) and
`20260908T134333-90367d4f` (Mali) each performed 384 readbacks over sixteen formats,
two shapes, four mip levels and three changed-input submissions. They exercise
sampling, swizzles, borders, GPU patches, raw-block preservation and sentinels.
Both report zero validation errors. Mali passes all readbacks, including 64
copy2 and 64 synchronization2 cases and the available maintenance4 path.
Those optional paths are unavailable on Redmi.

Redmi remains FAIL, with 12,524 mismatching words:

| Format | BC/native | Native/nearest golden | Native/filtered golden |
| --- | ---: | ---: | ---: |
| BC1 RGB UNORM | 153 | 0 | 0 |
| BC1 RGB sRGB | 153 | 0 | 0 |
| BC7 sRGB | 0 | 0 | 51 |
| BC6H UFLOAT | 153 | 0 | 6,069 |
| BC6H SFLOAT | 153 | 0 | 5,792 |

All raw-block, sentinel and auxiliary float32-nearest comparisons pass. The
BC6H filtered-golden failures occur in the native uncompressed reference;
they remain failures against this probe's ideal-filter bound. The BC/native
BC6H differences and full border semantics remain unresolved. These results
are not CTS coverage and do not prove a native-driver conformance violation.

## Probe packing diagnosis

The initial Redmi result included 516 unsigned and 1,095 signed nearest-golden
word differences. Auxiliary `floatBitsToUint(texelFetch(...))` comparisons in
run `20260908T134148-8143389d` all passed while those half-packed differences
persisted. This isolates loss of half subnormals to `packHalf2x16` in that probe
shader, rather than the texture fetch. Mali control
`20260908T134148-d3b7126a` passed both representations.

The probe now uses integer round-to-nearest-even conversion for binary16
subnormals and retains native packing for normal values. Signed zero is
preserved. With this correction, both devices pass all nearest-golden checks.
The original BC/native equality, filtering bounds and sentinel requirements
are unchanged; the remaining failures above are retained.

Performance, maximum resource sizes, reserved blocks, general image/view/copy
semantics and the outstanding G07 acceptance conditions remain unverified.

## Final entry-route regression

Force-mode runs `20260908T134515-e603dbcb` (Redmi) and
`20260908T134515-1ea45774` (Mali) use the final probe bundle and integrated
runtime. GIPA, GDPA, dlsym and linked image routes each perform 384 readbacks.
Mali passes all four routes; Redmi retains exactly the 12,524 differences
above in each route. All eight routes report zero validation errors. The
internal decoder and multi-UBO rendering pass on both devices. Scaled fallback
is disabled in these force-BC runs: Redmi returns UNSUPPORTED and Mali passes
its native path, so these runs do not establish Redmi scaled emulation.

Missing-mode runs `20260908T134706-c69f12e9` (Redmi) and
`20260908T134706-d4a661a7` (Mali) retain the same image results. Both pass
scaled-vertex and multi-UBO validation with scaled fallback set to missing;
Redmi exercises conversion, while Mali uses native formats.

Default-off runs `20260908T134750-4e7ad5e7` (Redmi) and
`20260908T134750-d55ce8dc` (Mali) pass native, frontend, ICD and ICD-validation
kernel routes: 344 readbacks per route, 2,752 total, zero differences and
validation errors. Caps and caps2 pass. Native/frontend/ICD image controls each
query all sixteen formats and return UNSUPPORTED; they provide query/control
evidence, not pixel coverage.
