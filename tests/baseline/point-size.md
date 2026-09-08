# Pipeline-scoped constant PointSize cleanup (2026-09-08)

`HYBRIS_VULKAN_COMPAT_POINT_SIZE=1` enables opt-in removal of a PointSize output
whose complete analyzed use consists of stores of the float32 constant 1.0.
It applies only to eligible non-point graphics pipelines with vertex/fragment
stages. The feature stays off by default and is ignored in secure processes.
It does not change the queried largePoints feature or device limits.

Point-list topology, point polygon mode, dynamic topology/polygon mode,
geometry/tessellation/mesh stages, pipeline libraries and extension-bearing
pipeline/stage/rasterization/input-assembly/dynamic-state or module creation
requests are excluded. PointSize reads, aliases, whole-block uses, non-one or
specialization-dependent writes, transform feedback, SPIR-V extensions and
unsupported annotation forms preserve the original shader. The predicate's
excluded dynamic/extended pipeline forms have not been exercised on-device in
this batch.

The implementation requires **all** uses of a candidate output to be understood;
it does not infer safety from finding one constant store. `compat/point_size.c`
owns the eligibility and use analysis. It removes eligible stores and pointer
instructions, then the shared output-declaration pass removes the now-unused
PointSize declaration and consistently remaps structure members and accesses.

The original application module is captured without mutation. Each eligible
pipeline receives a temporary transformed module; a subsequent point pipeline
created from the same application handle receives the original behavior.
The shared module/pipeline registry is now named `shader_dispatch.c`, reflecting
its ownership of scaled conversion and PointSize cleanup. The passes remain in
separate files. Unused clip/cull cleanup can also compose with this path.

## Independent rendering and shader evidence

The existing headless executable now includes `point-size` cases; no unit-test
framework was added. `shaders/generate-point-size.py` compiles three vertex
variants and one fragment shader, validating all generated SPIR-V:

| Variant | Source behavior | Triangle expectation | Point expectation |
| --- | --- | --- | --- |
| Constant | Writes PointSize=1.0 | 256 white pixels | One white pixel |
| Read | Writes 1.0 and reads PointSize into fragment color | 256 white pixels | One white pixel |
| Mixed | Writes 1.0, then conditionally writes 5.0 | 256 white pixels | 25 white pixels |

Each module is reused in triangle → point → triangle order with the same
pipeline cache. Every one of the 256 RGBA pixels is checked against a fixed
CPU mask on each of nine readbacks. This tests module ownership across topology
changes, point rasterization, read preservation and mixed-write preservation.

The point-size evidence checker requires exactly two transformations, both for
constant-one triangle pipelines. It checks the actual submitted source hash
against the generated constant fixture, validates before/after modules with
SPIRV-Tools, independently identifies the removed constant store and pointer,
checks member/type/decoration changes, and requires every other function
instruction to remain unchanged. No transformation may appear for the read,
mixed-write or point pipelines. The checker does not execute the production
rewriter as its reference.

An offline host compilation with warnings treated as errors validated a
constant-store transformation; separate read and mixed-store modules remained
byte-identical. Device runs `20260908T143637-489622f8` (Redmi) and
`20260908T143637-ad004320` (Mali) then passed GIPA, GDPA, ELF lookup and linked
routes with PointSize cleanup independently enabled. All eight routes completed
nine readbacks with zero pixel differences and zero Vulkan validation errors;
all actual shader artifacts passed the independent checks.

After those runs, the implementation added explicit exclusions for shader-module
creation extension chains and dynamic-state extension chains. The final clean
AArch64 build completed in 51.548 seconds, staging 17 runtime ELFs and checking
58 installed ELF paths. The final probe bundle used NDK `27.3.13750724`.

This establishes the stated constant-output optimization and its tested guards,
not a demonstrated application bug fix, arbitrary PointSize rewriting, active
clip/cull emulation, or completion of G08.

Final-build combination runs `20260908T144310-36e08a75` (Redmi) and
`20260908T144310-4021329a` (Mali) enable PointSize cleanup, unused clip/cull
cleanup and forced scaled conversion together. All four PointSize entry routes
pass, as do the reordered-builtin scaled fixture, multi-entry scaled fixture
and multi-UBO validation. The final submitted modules, including the composed
changes, pass their source and structural checks with zero validation errors.

Default-off controls `20260908T144406-60b1a2ca` (Redmi) and
`20260908T144406-03e451ca` (Mali) pass native bionic, frontend glibc and ICD
validation point-size cases, plus caps/caps2. These controls execute the same
nine pixel checks per point-size case with no cleanup transformation records.
They establish native/reference behavior and default-off regression coverage;
no previously failing application is claimed repaired by these results.
