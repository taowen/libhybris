# Packed SNORM vertex fetch

`HYBRIS_VULKAN_COMPAT_PACKED_VERTEX=1` enables an experimental static-pipeline
fallback for `A2R10G10B10_SNORM_PACK32`. It is off by default and ignored during
secure execution. The adapter adds only the vertex-buffer format bit, and only
when the source lacks that bit and native `A2B10G10R10_SNORM_PACK32` fetch exists.
`force` selects the same conversion even when the source format is native.
Image features and other packed formats are unchanged. When conversion is
selected, the [static vertex capability policy](capabilities.md#static-vertex-conversion-policy)
restricts optional paths that cannot use this conversion.

The pipeline uses native A2B SNORM fetch and a float32 vec4 Input. The shader
selects components 2,1,0,3, truncated to its original width. Scalar inputs extract
component 2. The original variable becomes Private storage initialized at entry,
so existing dynamic component access chains preserve their float values. Native
fetch supplies the signed normalization, including negative minima and the
two-bit alpha. This reuses the aggregate lowering pass; scaled integer conversion
skips these attributes. Source modules and allocation callbacks remain owned by
the application as before.

Run `tests/baseline/run.py --packed-vertex-compat missing` (or `force`) with the
usual `--icd-hal` and standard loader arguments. Select
`icd-scaled-vertex-packed1` through `packed4`, optionally with `-validation`;
matching `native-` cases provide the native controls. The generated shaders use
float, vec2, vec3 and vec4, with runtime component indexing for vectors. Every
format draws three full images: signed extrema, fractional values, and an
intentionally wrong expected red component. Both packed channel orders are
tested against independently computed CPU expectations. No pixel tolerance was
relaxed. The runner audits actual shader dumps with `packed_evidence.py`, checking
fixture bytes, spirv-val, the float vec4 fetch, exact component selection,
Private initialization and all six readbacks.

2026-09-08 AArch64 runtime and NDK probe builds completed. Final runs:

| Device / mode | Result directory | Evidence |
| --- | --- | --- |
| Mali X300 / missing | `20260908T152210-2fd86c1d` | Four native controls and eight ICD cases pass; native A2R is unsupported, so native cases execute only A2B |
| Adreno 650 / force | `20260908T152211-5de52041` | Four native and eight ICD cases pass; both native formats execute |
| Mali scaled / force | `20260908T152122-8e6d2d47` | Seven validation regressions pass |
| Adreno scaled / force | `20260908T152123-cc03f396` | Seven validation regressions pass |

The sixteen packed ICD cases include standard validation, zero validation errors,
zero live callback allocations, and independent audits of 32 dumped modules.
The scaled regressions cover ordinary vectors, arrays, matrices, matrix arrays,
nested arrays, specialization and grouped specialization with the existing
shader evidence checks. Native/default-off caps runs
`20260908T145358-790863cc` (Adreno) and `20260908T145358-2d344970` (Mali) reported
identical native/ICD packed-format buffer flags before enabling this option.

The fallback is not general packed vertex support. Nonzero Component inputs,
unsupported pointer/declaration forms, shader objects, graphics pipeline
libraries and dynamic vertex input are not implemented. Packed aggregate leaves
share the implementation but lack a dedicated packed aggregate device fixture.
Other packed formats continue to require native support.

Unmodified official Mesa on Mali creates the requested GL 3.3 core context with
this option (reports GL 4.4), but desktop GL drawing **fails**: Zink uses dynamic
vertex input, which the static conversion rejects. Result
`20260908T151633-2c99069e` retains the precise diagnostic, twelve packed images
with 256 bad pixels each, and the main draw failure. The option does not satisfy
desktop GL acceptance. That initial implementation did not restrict extensions.

The subsequent static vertex capability policy lets Zink select static pipelines.
Runs `20260908T154826-e58e4297` and `20260908T155655-425e451e` (the latter
with confirmed VVL and SyncVal activation) draw all six divisor=1 packed cases
correctly. All six divisor=2 cases still have 128 incorrect pixels; the main
draw reports failure because the packed subprobe fails. No validation error was
reported. The cause of the divisor failure remains unresolved, and desktop GL
acceptance at that stage remained **FAIL**. Mesa source is unchanged.


A temporary trace immediately before the ICD's backend CreateGraphicsPipelines
call in `20260908T160612-b2766ef1` and isolated-build repeat
`20260908T161056-c5371984` observes twelve one-binding packed pipelines with
instance-rate input and no vertex-input divisor `pNext` chain. Both retain the
same six divisor=2 failures. This narrowed the investigation to missing submitted
divisor state; the trace alone did not establish where that state was lost. The temporary
instrumentation was removed. Independent [dynamic binding stride](scaled-instancing.md#dynamic-binding-stride)
checks pass on Mali with explicit divisor state, including forced conversion,
but they use scaled formats and do not prove the failing GL workload correct.


The subsequent [KHR-to-legacy maximum query correction](capabilities.md#legacy-divisor-properties-on-khr-only-devices)
resolves the missing state: Zink was clamping the divisor to the zero returned
by its old EXT properties query. Run `20260908T162117-15b1d0b5` requests EGL
core 3.3 on Mali with `--packed-vertex 1` and reports GL 4.4. The main draw and
all twelve packed draws pass with zero bad pixels and zero GL errors. VVL and
SyncVal are explicitly active and report no validation errors. The official
Mesa commit and GL probe binary are unchanged. Earlier failed runs are retained.
This is the fixed offscreen workload; it does not establish full OpenGL 4.4,
GLX/window presentation, nonzero base-instance or every packed-format semantic.


Expanded run `20260908T162454-da900e1b` adds `--vertex-prepass --vertex-draws`
and retains validation. Packed draws and all three explicit compute-prepass
phases pass, but attribute phases 1, 3, 4, 5, 6, 7, 9 and 10 each fail all 256
pixels; phases 0, 2 and 8 pass. The probe stops there, so multidraw, indexed,
resource-budget and procedural cases are not executed. Vertex SSBO limit is
still zero (fragment/compute are 16 each). This expanded run is **FAIL** and its
remaining errors are not closed by the divisor-properties correction.


[Standard capture of the expanded workload](../desktop-gl/capture.md) now retains
the recorded pipeline/binding state and queue-submit references. Four failing
direct draws use divisor=2 with firstInstance=5 despite Mali's false KHR
nonzero-firstInstance property. Four indirect argument buffers still require
decoding. This is diagnostic evidence; the expanded workload remains failing.
