# Capability query probes

Native, replacement-frontend and ICD query comparisons. Feature/limit queries are distinct from execution coverage; format conversion policies have their own [evidence](scaled-format-policy.md).

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


## Static vertex conversion policy

When scaled or packed vertex conversion selects a nonzero format mask, the ICD
withholds `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_graphics_pipeline_library`
and `VK_EXT_shader_object`. Their features are false through both features2
aliases. Explicit extension requests fail with EXTENSION_NOT_PRESENT; enabling
a restricted feature without its extension fails with FEATURE_NOT_PRESENT,
before adapter allocations or the HAL create call. GDPA returns NULL for
CmdSetVertexInputEXT and the four shader-object commands. These restrictions
match the static converter's current limits; they do not implement those paths.
Default-off and enabled-but-zero-mask devices preserve native capabilities.
The feature query composes with BC policy when both options are enabled.

`vertex-policy` runs through native, frontend, standard-loader ICD and direct
ICD routes. Explicit `icd-vertex-policy-restricted` and
`icd-vertex-policy-restricted-direct` cases require a nonzero conversion mask.
They check extension enumeration counts and a truncated prefix, both features2
aliases, chain pointers, all 55 legacy feature values, a 16-bit-storage tail,
five GDPA results, six rejected creates and callback allocation balance. Direct
ICD rejection must invoke no allocation callbacks. The ordinary probe records
native capabilities without expecting restrictions.

A fresh runtime and NDK probe build passed on 2026-09-08. Device runs:

| Configuration | Mali X300 | Adreno 650 | Result per device |
| --- | --- | --- | --- |
| Packed missing / force | `20260908T155444-1bf9ca36` | `20260908T155444-a1b776ab` | 8 PASS |
| Default off | `20260908T160206-4b76a7f3` | `20260908T160207-8e4c2620` | 6 PASS |
| Zero mask: scaled missing / packed missing | `20260908T155932-41e5c955` | `20260908T155932-db682db9` | 7 PASS |
| Packed conversion plus BC missing | `20260908T160236-f26969a5` | `20260908T160237-722484da` | 5 PASS |

Native, ICD and direct ICD records of all three optional extensions/features
agree in default and zero-mask controls. Mali natively reports dynamic vertex
input and graphics pipeline library; Adreno reports neither. Neither reports
shader objects, so preserving a native true shaderObject feature is untested.
Active and combined runs include packed vec4 readbacks with validation and
shader-dump audits. The zero-mask Adreno audit requires no converted modules
while retaining all six pixel readbacks and negative controls. This capability
policy does not close the dynamic-input, pipeline-library or shader-object gaps.
The initial [desktop GL result](packed-vertex.md) still failed for divisor=2;
the property correction below addresses that separate failure.


## Legacy divisor properties on KHR-only devices

The KHR promotion introduced a distinct properties structure with
`supportsNonZeroFirstInstance`; it is not an alias of the old EXT properties
structure. See the [KHR proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_vertex_attribute_divisor.html).
On Mali, the native EXT query leaves an initially zero maximum at zero while
the KHR query returns 4294967295 and nonzero-firstInstance=false. Official Zink
at the pinned revision enables the KHR extension through its promotion handling,
but still queries the EXT properties structure. `zink_create_vertex_elements_state`
then clamps the requested divisor to that zero maximum and omits divisor state
from the static pipeline. This accounts for the earlier divisor=2 failures.

With a nonzero static vertex-conversion mask, properties2 core/KHR wrappers now
fill a zero EXT maximum from an independent native KHR query, only when KHR is
advertised and EXT is absent. Native EXT support, an already populated EXT field,
default-off and zero-mask paths are preserved. No EXT extension is added and
KHR's nonzero-firstInstance value is unchanged. This supports the legacy maximum
query; it does not implement the full EXT extension or nonzero-firstInstance.
Core-only Vulkan 1.4 devices without KHR advertisement are outside this fallback.

`vertex-policy` now checks both properties2 aliases, combined EXT/KHR/maintenance3
chains versus separate queries, pointer preservation, core property consistency
and the native KHR firstInstance flag. Restricted routes require the corrected
maximum on KHR-only devices. Fresh AArch64 runtime and NDK/glibc probe builds
completed; the following 2026-09-08 runs retain the deployed hashes and queries:

| Configuration | Mali | Adreno 650 | Result per device |
| --- | --- | --- | --- |
| Packed missing / force | `20260908T162138-9a01cabf` | `20260908T162139-fe639a1f` | 5 PASS |
| Default off | `20260908T162208-c34384b1` | `20260908T162210-4f458c39` | 6 PASS |
| Zero mask | `20260908T162300-d33b0205` | `20260908T162301-c08e069f` | 7 PASS |
| Packed plus BC missing | `20260908T162424-14de5f5a` | `20260908T162349-341315cc` | 5 PASS |

Default and zero-mask native/ICD/direct-ICD divisor records match exactly on each
device. Mali's active EXT maximum becomes 4294967295 while KHR's maximum and
false firstInstance flag remain intact; Adreno retains its native EXT maximum
65535. Active/combined runs also pass the existing features2 and packed vec4
validation/dump checks. A clean old-library control, `20260908T162348-1a073c53`,
fails both restricted property routes with the identical new probe binary
SHA-256 `5134566b0f5fa65c7b0b2528be39d3edd2f480e1309986c3a9d31d6b0895726b`.
Its version discovery still passes. The corrected library passes those routes.

The [packed GL probe](packed-vertex.md) now passes on Mali with actual VVL and
SyncVal activation; the expanded attribute workload still fails. This closes the reproduced maximum-query failure, not all
vertex-input, desktop GL or G05/G07/G10 acceptance requirements.
