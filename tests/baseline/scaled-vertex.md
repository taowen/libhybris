# Scaled vertex conversion evidence

The opt-in static vertex conversion and its vector, entry-point and literal-operand probes. [Aggregate inputs](scaled-aggregates.md), [grouped SPIR-V](spirv-groups.md), [instance divisors](scaled-instancing.md), [format decisions](scaled-format-policy.md) and [packed SNORM](packed-vertex.md) have separate evidence.

## Experimental scaled vertex conversion

`scaled-vertex` draws 12 formats (R/RG/RGBA, 8/16-bit, USCALED/SSCALED), each
with low/high integer patterns and an intentionally wrong expected value.
Every 16×16 RGBA component is checked: good draws are white and the negative
control is cyan. Missing components must become 0/1. One original vertex
module is reused across signed and unsigned pipelines. Shader and pipeline
allocation callbacks must return to zero live allocations after teardown.
`scaled-vertex-gdpa`, `scaled-vertex-elf` and `scaled-vertex-linked` change the
shader/pipeline entry route; ordinary device commands still use the loader.
`scaled-vertex-validation` and `scaled-vertex-gdpa-validation` enable VVL and
SyncVal. These are real GPU probes, not a new unit-test framework. The existing
allocator callback fixture is shared with `vk-alloc`.

The standard ICD option `--scaled-vertex-compat missing` activates only missing
scaled formats with supported integer replacements. `force` is a diagnostic
control for drivers with native support. Native and replacement-frontend cases
are unaffected by either option. The runner pulls the actual original and
converted modules used internally, validates each with `spirv-val`, retains
`spirv-dis --raw-id` output, matches the original hash to the probe build input,
and verifies location zero changes from float32 vec4 to the expected signed or
unsigned int32 vec4 without changing decorations or the selected entry's
interface. Unselected entry declarations are removed from temporary modules.
Each pipeline
has a location/signedness record. This is fixed-workload evidence; it does not
reconstruct arbitrary application pipeline or specialization history.

Reproduce, adding the device's existing `--icd-hal`, `--vulkan-loader` and VVL
arguments (the inspected Mali build receives its documented MMUD handling
automatically; older library builds require the explicit option):

```sh
python3 tests/baseline/run.py --serial DEVICE ... \
  --scaled-vertex-compat missing \
  --case icd-scaled-vertex --case icd-scaled-vertex-gdpa \
  --case icd-scaled-vertex-elf --case icd-linked-scaled-vertex-linked \
  --case icd-scaled-vertex-validation --case icd-scaled-vertex-gdpa-validation
```

2026-09-07 results for the final reviewed library:

- Redmi Adreno `20260907T163310-cfc97a46`, normal missing-format fallback:
  10 PASS (version, six scaled routes, UBO validation, device lifecycle and
  allocator regression). All 12 formats are converted, 36 draws per scaled
  route; VVL/SyncVal report zero errors and callback allocations return to zero.
- X300 Mali `20260907T163310-287ed5ac`, forced diagnostic fallback: the same
  10 cases PASS, with all 12 formats converted and the same pixel, shader and
  allocation checks. The driver natively supports these formats.
- Both runs retain 72 original/converted pairs apiece (12 pipelines × six
  routes), all validated and associated with the original probe shader.
- Disabled controls on the final library: Redmi
  `20260907T163603-a0e86d31` has version PASS and native/replacement/ICD scaled
  probes all UNSUPPORTED (12 missing formats). Mali
  `20260907T163603-53a349e5` has all four cases PASS.
- Mali normal missing-format mode `20260907T163408-f2366d30` has version and
  scaled validation PASS, `fallback_mask=0`, and no converted modules. It
  preserves native support rather than forcing conversion.

G07/G08 remain open. The production fallback supports direct scalar/vector
float32 Location inputs in the selected vertex entry point and a bounded set of
pointer operations; the [aggregate extension](scaled-aggregates.md) adds float32 matrices
and fixed arrays. General interface blocks and unhandled pointer control flow,
dynamic vertex input, graphics pipeline libraries, shader objects and shader
stage extension chains are unsupported. Offline mixed scalar/vector,
component-load and specialization-declaration rewriting passes `spirv-val`;
that extra fixture is not GPU specialization coverage. No shader/pipeline OOM-site sweep, arbitrary
application rendering, FormatProperties3 chain device probe, clip/cull,
point-size, BC or software timeline emulation is claimed. See
[the ICD contract](../../hybris/vulkan/icd/README.md) for the exact opt-in and diagnostic boundaries.

### Multiple entry points and shared stage modules

The `scaled-vertex-multi` family uses a single module containing two named
vertex entries and a fragment entry. Unsigned pipelines choose `scaled_vertex`;
signed pipelines choose `alternate_vertex`, whose output is inverted. Good and
negative-control draws therefore prove both entry selections. The same original
module supplies the fragment stage throughout all 12 pipelines. Each vertex
entry calls a helper that reads the push constant, exercising retained callee
and global-variable dependencies. Generate the assets with:

```sh
python3 tests/baseline/shaders/generate-scaled.py
```

This requires `glslangValidator`, `spirv-link`, `spirv-val`, `spirv-dis` and
`spirv-as`. The generated
normal vertex/fragment and three-entry arrays are checked in and copied into
the probe build snapshot. The ordinary single-entry fixture remains unchanged
at the binary level.

Adreno initially rejected pipeline linking even after unused entry declarations
were removed: `20260907T164648-f30ebfb1` returned `VK_ERROR_UNKNOWN`, and the
probe process's driver log reported a symbol-map assertion. A separate-fragment
control `20260907T165019-8c7ce314` also failed. The entry extraction now removes
unreachable functions, unused globals and dangling names/decorations, and
normalizes other multi-entry stages in the affected pipeline. The same original
module subsequently passed in `20260907T170117-21293b74`; failure and recovery
dumps have identical original SHA256
`fe7cc5f6808b35b62ccaf407f5dd46c8b98314082a32df98b00a0dbbb575b976`.
This comparison isolates the original workload; the final fixture adds helper
calls to cover the extraction pass's dependency handling.

Final helper-fixture runs on 2026-09-07:

- Redmi Adreno `20260907T170439-152289d9`: 11 PASS / 2 UNSUPPORTED. All six
  multi-entry ICD routes pass in normal missing-format mode; native/replacement
  controls lack all 12 scaled formats. Single-entry scaled validation, widget
  UBO validation, device lifecycle and allocator regression pass.
- X300 Mali `20260907T170439-336c2f68`: all 13 cases PASS, with forced conversion
  for ICD routes and successful native/replacement controls.
- Mali normal missing-format mode `20260907T170608-5b6f7fef` has version and
  multi-entry validation PASS, `fallback_mask=0`, and no temporary dumps;
  the native multi-entry path is preserved.
- Every multi-entry ICD route checks 36 complete 16×16 images, zero validation
  errors where enabled, and zero live callback allocations after teardown
  (352 Adreno / 1060 Mali allocation calls). Each pipeline emits a vertex and
  fragment original/temporary pair. Across six multi-entry routes and one
  single-entry regression, each device retains 156 audited pairs: source
  hashes, `spirv-val`, disassembly, selected entry and retained-interface
  decoration comparisons. Fragment interfaces remain floating point; only
  vertex inputs change to the corresponding integer type.

Use the existing runner arguments with `--case icd-scaled-vertex-multi`,
`icd-scaled-vertex-multi-gdpa`, `icd-scaled-vertex-multi-elf`,
`icd-linked-scaled-vertex-multi-linked`, `icd-scaled-vertex-multi-validation`,
and `icd-scaled-vertex-multi-gdpa-validation`. The compatibility option remains
explicit. This does not establish general multi-entry normalization outside
scaled pipelines. Unknown opcodes, function pointers, unhandled debug references,
general interface layouts, OOM injection at each temporary-stage allocation,
and specialization/cache-key coverage remain open. G07/G08 are not closed.


### Literal operands that overlap variable IDs

The `scaled-vertex-literal` fixture gives the Location 0 input SPIR-V ID 3 and
uses vector shuffle component literals `3 2 1 0`. Input and expected values are
swizzled before comparison, and the result is swizzled back, preserving the
existing white/cyan pixel checks. The shader generator changes only numeric ID
tokens through SPIRV-Tools disassembly/assembly; it leaves component literals
unchanged and validates the resulting module. Existing single/multi-entry
shader bytes remain identical. Runtime evidence also verifies that the original
input is `%3` and the shuffle index overlap is present.

The old adapter falsely treated the literal as an unsupported pointer use.
Archived-library Adreno run `20260907T171838-a7a012f4` reproduces rejection of
the first pipeline with `VK_ERROR_UNKNOWN` and the pointer-use diagnostic.
The new and old runs use identical probe binaries, verified against their
manifests. This is an intentional failing negative control, not a passing
acceptance result.

Fixed scans use `spirv_literals.inc`, generated alongside the result-ID table
from the same hash-pinned Khronos grammar. The 203 records describe definite
literal positions and literal-only suffixes, including shuffle/extract indices,
branch weights and fixed enum words. Both the vertex-pointer scan and the
multi-entry global-liveness scan use this metadata. Parameterized enum payloads,
composite operand pairs, variable-width strings and other ambiguous layouts
remain conservatively scanned; this is not a complete operand parser.

Final 2026-09-07 runs:

- Adreno `20260907T172002-4a50d24f`: 12 PASS / 2 UNSUPPORTED. Six literal-fixture
  ICD routes pass with normal missing-format conversion. Native/replacement
  controls still lack the scaled formats. Single-entry, multi-entry and UBO
  validation, device lifecycle and allocation regressions pass.
- Mali `20260907T172002-0b12fec3`: all 14 cases PASS, using forced ICD conversion
  plus native/replacement controls.
- Each literal route checks all pixels for 12 formats × three phases, with
  zero validation errors where enabled and zero live callback allocations
  (284 calls on Adreno, 992 on Mali). Each device retains 108 audited
  original/converted module pairs across the literal and regression fixtures.
  Both generated tables reproduce byte for byte, and production sources match
  the tested build snapshot. The ICD still exports only its three loader entries.

Use `icd-scaled-vertex-literal`, `icd-scaled-vertex-literal-gdpa`,
`icd-scaled-vertex-literal-elf`, `icd-linked-scaled-vertex-literal-linked`,
`icd-scaled-vertex-literal-validation` and
`icd-scaled-vertex-literal-gdpa-validation` with the existing opt-in runner
arguments. General operand layouts, interface forms and the remaining G07/G08
acceptance requirements remain open.
