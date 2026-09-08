# Scaled vertex input with instance divisors

The experimental scaled fallback now preserves
`VkPipelineVertexInputDivisorStateCreateInfoEXT` (including its KHR/core aliases)
on the copied vertex-input state. Replacing scaled fetch with integer fetch
uses the same bytes, stride and instance cadence. Previously any vertex-input
`pNext` caused `VK_ERROR_UNKNOWN`, even for a supported divisor of one.
Unknown vertex-input extensions still fail conservatively. This change neither
advertises nor emulates divisor capabilities.

The independent probe uses `scaled.divisor.vert` for four modes:

| Mode suffix | Divisors | firstInstance | Pipelines / readbacks across 12 formats |
| --- | --- | --- | --- |
| `divisor` | 1, 2, 3 | 0 | 36 / 108 |
| `divisor-zero` | 0 | 0 | 12 / 36 |
| `divisor-base` | 1, 2, 3 | 3 | 36 / 108 |
| `divisor-zero-base` | 0 | 3 | 12 / 36 |

Each draw has four instances, each drawing two triangles in one quarter of a
16×16 image. Distinct data rows near both signed/unsigned endpoints check the
fetch index `firstInstance + floor(relativeInstance / divisor)`; zero divisor
repeats the firstInstance row. CPU expectations include missing-component
zero/one defaults. Two phases must produce white; a deliberately wrong
expected red component in every instance must produce cyan. Every pixel and
channel is checked, including the three bands after the first instance.

`scaled_divisor.h` holds capability negotiation and data construction outside
the rendering setup. It selects the advertised KHR extension or EXT revision 3,
queries divisor and zero-divisor features and the maximum divisor, and enables
only the needed features. For KHR it also queries
`supportsNonZeroFirstInstance`; EXT guarantees that behavior. Unsupported
combinations exit before device creation. The distinction follows the
[KHR divisor proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_vertex_attribute_divisor.html)
and the [EXT extension history](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_vertex_attribute_divisor.html).
Core-only divisor advertisement without either extension is not exercised.

For example, add these cases to the standard runner's loader/HAL options:

```sh
--case native-scaled-vertex-divisor \
--case icd-scaled-vertex-divisor \
--case icd-scaled-vertex-divisor-validation
```

Select the other suffixes separately. Validation cases require the layer,
layer JSON and build manifest described in [the loader guide](loader-tools.md). Use
`--scaled-vertex-compat missing` for the Adreno fallback; the Mali forced
conversion control uses `force` and the existing scoped loader quirk.
The shader auditor verifies capabilities, the full case/divisor/draw sequence,
all readback records and the fallback-mask-dependent module count. Original
modules must match the built fixture hash; original and converted modules pass
spirv-val, retain decorations and expose the expected float-to-integer input
change. An unsupported case has capability evidence and no module/draw claims.

Actual AArch64 library build and Bionic/glibc/linked probe builds succeeded.
The following runs retain manifests, staged binaries, logs and shader audits
under `build/results/`:

| Run | Result and scope |
| --- | --- |
| `20260907T190014-771ade7f` | Adreno 29854870: 14 PASS / 4 UNSUPPORTED. All four converted modes pass with and without validation; 96 pipelines / 288 readbacks per set. EXT maximum 65535, zero divisor supported. The four native controls find all 12 scaled formats unsupported and perform no draws. |
| `20260907T190016-f9c25f6d` | Mali 10AFA31610002QH: 9 PASS / 9 UNSUPPORTED. Native, forced conversion and forced conversion with validation each pass the basic divisor mode, 36 pipelines / 108 readbacks. KHR maximum 4294967295, zero divisor and nonzero firstInstance both false. Each path accurately skips the other three combinations. |
| `20260907T190058-ae47a950` | Mali missing mode: 2 PASS / 3 UNSUPPORTED, including version discovery. Basic divisor mode passes with fallback mask zero and no converted modules; the same three capability combinations remain unsupported. |
| `20260907T190057-e945563c` | Old library control: version PASS, divisor case FAIL. The prior library from `20260907T184150-a31e3934` rejects the first legal pipeline (divisor 1) with -13 before any shader dump/readback. The auditor also rejects the incomplete execution. |

Both primary runs also pass ordinary scaled, specialization, single/multi-entry
decoration-group and grouped-specialization validation regressions. Successful
rendering cases report zero pixel failures and zero validation errors. The old
and new Adreno runs use the identical probe-glibc SHA-256
`f4be2ca5ad11abe2711b08eefbb3f48f538d60b95ae19076ea3b3b7be818d805`;
the old/new ICD hashes are respectively
`3ea752aae63a1c1df8b04c83ce0322d5e611ed478ce67979f6362fe46c0499f1` and
`5c36251995dece214959d1253dc88e5466ad8964bb9cf2b5731296fed0e15f54`.

This covers static instance-rate input on the two vendor drivers. It does not
cover dynamic vertex input, graphics pipeline libraries, indirect draws,
multiple instance-rate bindings, maximum-divisor stress or full applications.
Mali zero/nonzero-first cases remain untested because the device does not
support them. BC/swizzle/sRGB and general SPIR-V compatibility gates remain open.


## Dynamic binding stride

`scaled-vertex-divisor-stride` combines the existing divisor=1/2/3 shader with
`VK_EXT_extended_dynamic_state` and `vkCmdBindVertexBuffers2EXT`. Pipeline stride
is dynamic. Each phase scatters eight distinct records behind a four-byte offset
with guard padding; phase 1 uses a different stride. Before every draw, the probe
sets a larger stride and then overwrites it with the correct stride. It reuses
each pipeline for three phases, including the existing deliberately wrong
expected-red control. This checks the last command's stride, instance cadence,
offset and pipeline reuse together. It does not use dynamic vertex-input formats.

The runner registers native, frontend and ICD cases plus the ICD `-validation`
variant. Capability negotiation precedes device creation; a missing extension or
feature returns UNSUPPORTED. The shader auditor checks all 108 stride records,
all 108 complete-image readbacks and the actual conversion modules. Its hash is
recorded in run metadata. Select the cases with the existing loader arguments:

```sh
--case native-scaled-vertex-divisor-stride \
--case hybris-scaled-vertex-divisor-stride \
--case icd-scaled-vertex-divisor-stride \
--case icd-scaled-vertex-divisor-stride-validation
```

Fresh AArch64 runtime and Bionic/glibc probe builds completed on 2026-09-08.
The runtime was rebuilt after removing temporary pipeline instrumentation.

| Device / mode | Run | Result |
| --- | --- | --- |
| Mali X300 / force | `20260908T161318-ed05bd23` | 6 PASS: four dynamic-stride routes, static-divisor validation regression and version discovery |
| Adreno 650 / missing | `20260908T161319-ed41850d` | 2 PASS / 4 UNSUPPORTED: version and static-divisor validation pass; extended dynamic state is absent |
| Mali X300 / missing | `20260908T161418-f530e91b` | 3 PASS: two ICD dynamic-stride routes and version; zero conversion mask, no shader dumps |

Each successful rendering case executes 36 pipelines and 108 full-image
readbacks with no pixel failures; validation cases report zero errors. The two
converted Mali dynamic-stride cases validate 144 original/converted modules in
total. Adreno's unsupported dynamic cases create no device or shader modules.
These observations cover one binding, direct draws and EXT command dispatch.
Core command aliases, indirect draws, multiple bindings, zero divisor with
dynamic stride, maximum strides and the packed-format combination remain open.
