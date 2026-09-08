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
