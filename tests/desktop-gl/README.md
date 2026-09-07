# Desktop OpenGL through Zink and hybris

This builds an actual desktop GL frontend: source-built Mesa/Zink → standard
glibc Vulkan loader → hybris ICD → Android vendor Vulkan HAL. EGL is Mesa's
implementation, not the hybris GLES EGL frontend. The initial integration uses
surfaceless EGL pbuffers, with no X server, APK or compositor. GLX pbuffer contexts are also available with an external X server, as described
below. A working Blender renderer and displayed desktop GL windows remain open.

Build prerequisites are the parent Ardesk checkout's clean Mesa revision
`080a97977a453a9d4b2eea59426c9ee84df19007`, its AArch64 cross file, Podman and
its existing GL cross-builder. The default cached builder image is
`localhost/ardesk-glibc-arm64:20d8189233233158`; `BUILDER_IMAGE` can select an
explicit available replacement. The script records the resolved image ID,
compiler, Mesa commit, probe/source hashes and packaged ELF hashes. It rejects
a different or dirty Mesa checkout. This dependency is not bundled into an
independent libhybris clone. The recorded image identifies this build; it is
not a new independently reproducible container recipe.

```sh
tools/build-aarch64.sh
tests/desktop-gl/build.sh
ADB=/path/to/adb python3 tests/desktop-gl/run.py \
  --serial 10AFA31610002QH --hal /vendor/lib64/hw/vulkan.mali.so \
  --api-version 1.3.305 --mali-loader-quirk --profile core32
```

`--api-version` must match baseline ICD version discovery for the selected
HAL, not a desired Vulkan version. Mali's existing explicitly enabled,
build-ID-scoped loader workaround remains necessary on this tested driver.
The supported probe choices are `core32`, `compat32`, and `core33`. The latter
checks a separately requested context; it does not silently fall back. Only
EGL_BAD_MATCH on context creation is recorded as UNSUPPORTED. Other failures
remain FAIL/CRASH/TIMEOUT, and all non-PASS results return a failing host exit.

The build enables only the Zink Gallium backend and no native Mesa Vulkan
ICDs. It installs Mesa into a fresh staging tree and collects the transitive
ELF dependency closure, preserving executable modes. Build/runtime files and
results stay under ignored `build/`. Compilation is cached by Ninja; remove
`build/mesa` for a clean rebuild or when changing build configuration. The
script does not reconfigure an existing build directory or support concurrent
builds in that directory.

The runner validates Mesa and hybris manifests, stages an independent shell
UID directory, forces the one explicit hybris ICD JSON and runs with the Mesa
runtime library directory before hybris. It does not set GL/GLSL version
advertisement overrides. The program requests a desktop context, compiles GLSL
1.50 vertex/fragment shaders, creates a VAO, uses gl_VertexID for a fullscreen
triangle and draws two different colors with gl_FragCoord. It reads all 256
RGBA pixels; both C and Python check the exact expected image. It rejects a
non-Zink renderer. Host acceptance also requires mappings for Mesa Gallium,
the standard loader, hybris ICD and selected Mali/Adreno HAL. Raw maps, image,
command and staged ELF hashes are retained. Mapping names/hashes of deployed
files are not an audit of driver memory contents or every dynamic load.

The probe has a 45-second alarm and a 60-second host watchdog. Cleanup checks
the recorded process PID's working directory before killing it, then deletes
only this run's unique directory. Device state is not otherwise reset. No unit
test framework is added.

Initial results on 2026-09-07, Mesa 26.3.0-devel at
`1cb7f0a1c9a5438045f89ad4aa83eda8fbafa09e` (before the fixes below):

| Device / request | Result | Evidence |
| --- | --- | --- |
| X300 / Mali-G1-Ultra MC12, GL 3.2 core | PASS | `20260907T104023-a4d52adb`, 256 exact pixels, no GL error |
| Same device, GL 3.2 compatibility | PASS | `20260907T104042-984ffd94`, 256 exact pixels, no GL error |
| Same device, GL 3.3 core | UNSUPPORTED | `20260907T104053-34894e46`, EGL_BAD_MATCH (0x3009) |
| Redmi / Adreno 650, GL 3.2 core | FAIL | `20260907T104023-86f66a86`, EGL_NOT_INITIALIZED (0x3001), failed to create DRI2 screen |

Successful contexts report Mesa/Zink, Vulkan 1.3 Mali, GL 3.2 and GLSL 1.50.
The Mesa ELF SHA256s are libEGL
`0d0bad6286e2b3d44971c6cc75401666aa952944aad7df3e6b119f781f56389a`
and libgallium
`2c843dca29b2cb47066f442578ac6b24f3e3e9c9f28efad4fd5f78913c55f770`.
Builder ID is `443690e01138637d8624d70fef6601444727044dd33e35136615aa29eb3a9678`.
Earlier core/compat runs also passed. Initial staging lost executable modes
and failed before loading; the final copy operations preserve those modes.

Correction to the initial diagnosis: the generated device code already
recognizes KHR_vertex_attribute_divisor and enables that KHR name. The first
GL 3.3 blocker was packed 10/10/10/2 vertex fetch, confirmed by the missing
GL_ARB_vertex_type_2_10_10_10_rev in the actual GL extension list. A separate
bug remains in the old code's divisor **property structure**: it uses the EXT
sType on a KHR-only driver. The corrected behavior is documented below.
The Adreno baseline
also lacks timeline semaphores and maintenance5 required by this Zink revision;
its EGL initialization failure was not independently isolated to one missing
capability. No feature is emulated or falsely advertised by this integration.

This establishes working desktop GL contexts and a small shader draw through
hybris on one device, not complete OpenGL 3.2 conformance or Gladio/Vortek
feature parity. UBO/SSBO, textures, FBOs, sRGB, instanced attributes, legacy
fixed-function drawing, GLX/window presentation, validation under Zink and
Blender are still unverified here. The old headless Vulkan/GLES suite was not
rerun: hybris production libraries were unchanged. Next work should address
the actual Zink capability/alias blockers and application drawing, rather than
expanding unrelated diagnostics.

## Packed vertex formats and KHR divisor properties

Mesa `2e3d35e` adds the eight packed RGBA/BGRA 10/10/10/2 normalized/scaled
formats to its existing u_vbuf CPU translation table. Unsupported inputs are
translated into R32G32B32A32_FLOAT; state-tracker extension queries check that
same translated destination against actual driver support. Supported native
formats keep their native path. This changes desktop GL vertex fetching, not
Vulkan format queries or compressed texture support. CPU conversion can cost
uploads and time; performance is not measured here.

Mesa `37f170c789f75792c209fa9221f7ab67bbd5b87f` separately fixes KHR divisor
properties. Its KHR struct has a different sType and an additional field, unlike
the aliased feature struct. The generator now queries the KHR property struct
when that extension is used and copies maxVertexAttribDivisor into the existing
state. EXT-only devices retain the EXT query; Vulkan 1.4 retains its core
property route. KHR supportsNonZeroFirstInstance behavior is not covered by
these zero-base-instance draws.

The strengthened probe performs twelve additional draws: signed/unsigned,
normalized or scaled RGBA, normalized BGRA, each with divisor 1 and 2. Two
instance records differ, and guard words surround offset-4/stride-8 input.
The shader compares all four fetched components against independent expected
values, including negative signed values and alpha, before outputting red/green
halves. Every draw first clears blue so a skipped draw cannot reuse a previous
passing image. Each checks all 256 pixels and GL errors; host acceptance also
requires all twelve unique successful case records. This is six GL input
configurations, not separate coverage of all eight internal pipe formats;
scaled BGRA is not a legal glVertexAttribPointer combination.

Evidence on X300:

- Original Mesa rejected core 3.3 (`20260907T104053-34894e46`). Actual extension
  enumeration `20260907T104507-85166d8c` includes ARB_instanced_arrays but lacks
  ARB_vertex_type_2_10_10_10_rev, correcting the earlier name-alias diagnosis.
- Packed conversion alone allows core 3.3, but distinct-data divisor-2 cases
  fail 128 pixels each (`20260907T105012-3a49ec45`). Temporary backend logging
  (`20260907T105206-0ff75bc3`) identifies requested divisor 2 clamped through max=0 and dynamic input using
  divisor 1; its exact diagnostic patch is stored in that diagnostic run's
  manifest. Those temporary logging edits were removed before the final build.
  Earlier identical-instance data was too weak to detect this defect.
- Final `20260907T105351-1b2678ab` core-3.3 request and
  `20260907T105419-ad7a9372` compatibility-3.2 request both pass all twelve
  packed draws plus the original draw. Both report Mesa GL 4.4 / GLSL 4.40.
  This reported version is not evidence of complete GL 4.4 conformance.
- Redmi `20260907T105351-89b27f4b` still fails EGL initialization; no EXT-only
  execution of the new packed draws or core-1.4 property route is established.

The final Gallium SHA256 is
`bc1dccce9a8727645170edc6a9e5b7483495b368a0e51f4b49a02f748eafe79c`.
Builds use the same recorded builder and actually recompile the modified Mesa
code and probe. Both Mesa fixes are source commits in the dependent repository,
and this directory's build pins their final revision. Unaligned access beyond
the specified layout, indexed/base-instance/indirect draws, packed value
boundaries, all compatibility fixed-function paths, Zink validation and Blender
remain unverified. No API version override is used.

## GLX frontend and Blender startup (2026-09-07)

The build now includes Mesa's X11/DRI GLX frontend and stages `libGL.so.1`,
its X11 dependencies and the installed DRI loader stub. The same stub is staged
as `swrast_dri.so` for Mesa's drisw frontend; `GALLIUM_DRIVER=zink` still selects
GPU rendering. `LIBGL_KOPPER_DISABLE=true` permits X11 transport without Vulkan
WSI or DRI3. This is a CPU window transport, not llvmpipe rendering. Do not set
`LIBGL_ALWAYS_SOFTWARE`: it requests a CPU Vulkan device. The GLX runner also
omits the EGL path's `MESA_LOADER_DRIVER_OVERRIDE=zink`, which would require DRI3
on the test X server. Runtime manifest hashes now include the DRI subdirectory.

An independent host Xvfb was used, with only its Unix socket exposed to the
phone through an ADB reverse (no changes to the running Ardesk desktop):

```sh
Xvfb :181 -screen 0 960x640x24 -nolisten tcp -ac -noreset
# In another terminal; use an unused display/port and remove this reverse afterward.
adb -s 10AFA31610002QH reverse tcp:6181 localfilesystem:/tmp/.X11-unix/X181
python3 tests/desktop-gl/run.py --serial 10AFA31610002QH \
  --hal /vendor/lib64/hw/vulkan.mali.so --api-version 1.3.305 \
  --mali-loader-quirk --profile core33 --display 127.0.0.1:181
adb -s 10AFA31610002QH reverse --remove tcp:6181
```

Final GLX runs `20260907T111445-3d509593` (core 3.3) and
`20260907T111446-436745c5` (compatibility 3.2) both pass. The GLX context code
is separate from the shared draw checks. Successful runs
require the full image and all twelve packed vertex cases, plus mapped staged
libGL, Gallium, standard Vulkan loader, hybris ICD and vendor HAL. These are
pbuffer tests, not validation of swaps, resize or visible application rendering.
The existing surfaceless EGL core-3.3 regression also passed in
`20260907T111349-36d45a8a` after the shared-draw refactor.

The actual installed Blender 4.3.2 was additionally launched under the Ardesk
app UID using its rootfs dependencies, the newly built GLX runtime and a fresh
HOME/config directory. It displayed the unsupported-platform dialog identifying
Mesa/Zink and Mali, then logged:

```text
Warning: Unsupported platform as it supports max 0 SSBO binding locations
```

Local evidence is in `build/blender/run-67256c7b/` (`command.txt`, `blender.log`,
`platform-unsupported.png`, staged libraries). The attempted Python draw check
never ran and produced no result. This is a failed application startup, not a
Blender rendering pass or a reproduction of the earlier widget corruption.
The executable reports 4.3.2; the local Blender source is only explanatory,
not proof that the installed binary was built from that exact revision.

Actual GL queries report SSBO limits vertex=0, fragment=16, compute=16 with no
GL error. Mesa's `zink_screen.c` suppresses vertex-stage shader buffers when
`vertexPipelineStoresAndAtomics` is false, matching the previously recorded Mali
Vulkan capability. Blender's `gl_backend.cc` requires a minimum of 12 across
these three stages. Correct vertex SSBO support/emulation remains necessary;
no feature or GL version was overridden to bypass the check. Redmi GLX, Zink
validation, full GL conformance and useful Blender rendering remain unverified.

## Compute vertex prepass feasibility

`--vertex-prepass` additionally exercises an explicit compute → vertex-fetch
path on the existing GL runtime. It requires actual GLSL 4.30 support. This is
preparatory evidence for the missing vertex SSBO implementation, not automatic
translation of application vertex shaders and not a capability override.

The compute shader reads twelve separately bound SSBOs, generates six position/
color records representing two triangle instances, writes an output SSBO and
updates a separate SSBO with atomic count/ID bits. Eight compute invocations
include two padding invocations that must not write. Guard records surround the
output. A vertex shader fetches the generated records as ordinary attributes;
it does not use vertex SSBOs. Explicit shader-storage, vertex-attribute and
buffer-update barriers cover subsequent dispatch, draw and CPU readback use.

Three phases reuse the same allocations. The first reads twelve distinct input
values and draws red/green halves; the second changes the twelfth buffer and
must draw magenta; the third restores it and must restore the original image.
Each phase clears blue before drawing. Acceptance requires all 256 pixels per
phase, unchanged guards, cumulative atomic counts 6/12/18, ID mask 63 and no GL
error. The runner also retrieves and independently compares all three RGBA
files. The original draw and all twelve packed-input cases remain required.

```sh
python3 tests/desktop-gl/run.py --serial 10AFA31610002QH \
  --hal /vendor/lib64/hw/vulkan.mali.so --api-version 1.3.305 \
  --mali-loader-quirk --profile core33 --vertex-prepass
```

This verifies the underlying compute/write/atomic → vertex-fetch route on Mali.
It does not implement NIR vertex-to-compute translation, automatic descriptor
remapping, indexed/base-instance/indirect draws, tessellation/geometry stages,
transform feedback or application shader specialization. Mesa's existing
`poly_nir_lower_sw_vs` is part of Asahi's software geometry path and relies on
its parameter-buffer/input-lowering ABI; it is not directly usable as a Zink
fallback. The Gallium draw interpreter is another possible implementation
route but is likewise not currently attached to Zink. Vertex SSBO limits remain
unchanged, and Blender startup remains failed. CPU readback synchronizes these
checks; this is not proof of a fully asynchronous production fallback.

Final source-built runs `20260907T112336-d7306baf` (core-3.3 request, actual
4.4 core) and `20260907T112337-5d47f3ed` (compatibility-3.2 request, actual 4.4)
pass all three prepass phases and the existing draws on X300. Artifacts are
under `build/results/`, with runtime/source hashes and full per-phase images.
No other device or GLX execution of this new workload has been established.

## Automatic procedural vertex-to-compute lowering

Mesa `6bec718` adds an actual Zink conversion path behind
`ZINK_DEBUG=vertex_prepass`. Eligible application vertex NIR is cloned and
converted to compute; a generated read-only vertex shader replays its outputs.
The application supplies an ordinary vertex shader, not a hand-written compute
replacement. Shader preparation and draw orchestration live in the separate
`zink_vertex_prepass.c` dependency module. The internal output SSBO and parameter
UBO use reserved slot 15, and occupied slots/unsupported draws are excluded.
The draw restores compute programs and buffer bindings, explicitly orders the
output write/read, and uses the existing deferred resource-release mechanism.

`--vertex-execution native|compute` runs the same additional application shader
with or without conversion. It reads a 272-byte std140 widget-shaped UBO
(parameters@0, mat4@192, vec3@256, int@268) and a default-block uniform. Three
instanced draws use first vertex 7 and base instance 5, with two distinct,
aligned UBO ranges. Changing the selected range makes the image magenta;
updating its final int restores red/green halves. All 256 pixels are checked
in every phase in both C and the host. Compute mode additionally requires
execution markers for the original draw and all three instanced draws. Shader
dumps are retained in both modes.

For example, after the pinned build and baseline dependency build:

```sh
python3 tests/desktop-gl/run.py --serial 10AFA31610002QH \
  --hal /vendor/lib64/hw/vulkan.mali.so --api-version 1.3.305 \
  --mali-loader-quirk --profile core33 --vertex-execution compute \
  --vertex-prepass \
  --validation-layer /path/to/libVkLayer_khronos_validation.so \
  --validation-manifest /path/to/VkLayer_khronos_validation.json
```

The optional validation arguments stage the supplied standard glibc layer and
its matching JSON. Acceptance requires its mapped ELF, an explicit startup
message confirming synchronization validation, and no reported validation
errors. The Vulkan loader performs normal layer/ICD loading; no private layer
chain is fabricated.

Final clean-source X300 runs, all including the previous explicit compute
workload, packed-input matrix and original draw:

| Execution | Local result directory | Result |
| --- | --- | --- |
| EGL core, native | `20260907T120708-6a393bfe` | PASS |
| EGL core, automatic compute | `20260907T120708-e70bbe07` | PASS |
| EGL compatibility, automatic compute | `20260907T120708-3e9771cf` | PASS |
| GLX core, automatic compute | `20260907T120708-1e054fb0` | PASS |

All three procedural images match across all four paths. Khronos layer
1.4.309 reports synchronization validation enabled and no errors in each run.
The 9 native and 17 modules per converted run (60 total) pass
`spirv-val --target-env vulkan1.3 --uniform-buffer-standard-layout`; the layout
flag permits the UBO layout used by this Mesa device configuration, whose
runtime use is also checked by validation. Per-run `spirv-validation.json`
records tool version, commands, hashes and results, with `.spvasm` disassembly.
Every generated vertex storage-buffer variable is decorated NonWritable.

Development failures retained in local artifacts explain two lifetime fixes:
`20260907T114252-9995beed` reports a leaked parameter buffer/allocation because
restoring an empty UBO struct does not unbind it. Restore now passes NULL.
`20260907T115944-72e5f374`, with a backtrace in
`20260907T120317-64f0ef5c`, crashes when restoring old compute SSBOs after the
application has deleted their buffers. Intermediate unbinding must preserve
Zink's deferred ownership until original bindings are restored. The final
combined workload covers that sequence. The initial shader-cache base-layout error was also fixed before validation.
Temporary resource-print/backtrace diagnostics are absent from the final build.

This remains a development path. It does not raise GL limits or Vulkan
features, and full vertex SSBO support is still absent. Currently excluded
are unaligned/64-bit or dynamically indexed vertex attributes, textures/images,
bindless and subgroup operations,
clip/cull arrays, transform feedback, indexed/indirect/multidraw, geometry and
tessellation stages, active queries and draws exceeding dispatch/storage limits.
Native execution remains in use for excluded draws. Programs and output storage
are created per draw; general caching, complete lowering, performance work and
application vertex SSBO acceptance remain open. No Blender startup/rendering
pass, full GL conformance, other-device conversion or complete G08/G10 closure
is claimed.


## Automatic vertex input pulling (2026-09-07)

Mesa `8986040` extends the experimental prepass to word-aligned vertex inputs.
Zink retains the Gallium layout before Vulkan attribute decomposition and
binds the original VBO storage to available compute SSBO slots. It uses Mesa's
`nir_format_unpack_rgba` for decoding and default components, with offsets,
strides, first vertex and `base_instance + instance / divisor` addressing.
There is no CPU readback, new Vulkan feature declaration or GL limit increase.
Application SSBOs and fetched attributes together must fit slots 0–14; output
still occupies slot 15. Unsupported layouts, unaligned fetches, out-of-range
fetch bounds or insufficient descriptor capacity use native execution. This
is still per-draw program/output allocation, not a general performant solution.

`--vertex-execution` now additionally runs `attribute_draw.c`: four simultaneous
inputs (float32 position, normalized four-byte color, signed SHORT pair and
half-float pair), distinct VBOs, guarded offsets/strides, first vertex 7, base
instance 5, four instances and divisors 1/2. The half pair is consumed as vec4
to check default z/w = 0/1. Both C and host check all 256 pixels in each image.
Compute execution requires two four-input conversion markers and all twelve
one-input packed conversions, so a silent native fallback cannot pass the
conversion gate. The packed GL formats may be converted to float by u_vbuf;
the new direct formats exercise integer sign extension, normalized conversion
and half decoding in the generated compute SPIR-V itself.

The strict pinned build passed. Final compute runs:

| API | Result directory | Outcome |
| --- | --- | --- |
| EGL core 3.3 | `20260907T122645-faf5dc1e` | PASS |
| EGL compatibility 3.2 | `20260907T122646-faddf6cf` | PASS |
| GLX core 3.3 | `20260907T122648-4e866e58` | PASS |

All three include the manual compute/deletion workload, packed matrix and
procedural UBO changes. Khronos validation confirms SyncVal enabled with no
reported errors. Each retains 64 SPIR-V modules, all passing `spirv-val` for
Vulkan 1.3 with uniform-buffer-standard-layout; disassemblies and exact tool
commands/results are retained in each result directory. They report vertex
SSBO limit 0; Blender acceptance remains blocked by the existing capability
gap. GLX used private Xvfb :183 with a temporary ADB reverse, removed afterward.

**Native counterexample retained:** `20260907T122644-031d5f2e` passes divisor 1
but fails every pixel of the new base-instance-5/divisor-2 case. Independent
native repeat `20260907T122849-e644af24`, without the manual compute workload,
reproduces it with GL error 0. No validation error is reported, but these are
FAIL, not native parity passes. The intended address formula agrees with the
[Khronos vertex-fetch specification](https://github.khronos.org/Vulkan-Site/spec/latest/chapters/fxvertex.html).
At that point the cause was not isolated between Zink state and vendor fetch
behavior. The following section resolves this counterexample through KHR
nonzero-first-instance capability handling; the original FAIL artifacts remain.
The first native run's nine SPIR-V modules also validate, which does not prove
runtime attribute fetching correct. Full G07/G08/G10/G13 acceptance stays open.


## Native first-instance rebasing (2026-09-07)

The native counterexample above came from an ignored device limitation.
Diagnostic build `20260907T123701-fe4e1c58` queries KHR divisor properties:
`maxVertexAttribDivisor=4294967295`, `supportsNonZeroFirstInstance=0`. Zink had
copied only the maximum divisor into its EXT-compatible state. Its dynamic
binding was correctly set to divisor 2, but it still submitted firstInstance 5.
An instrumented attribute shader observed source indices 2,3,3,4 instead of
5,5,6,6. Earlier `20260907T123440-6ad25b5b` records the same fetched values.
The zero capability means that combination is unsupported, not that a different
fetch formula is valid; see the [KHR divisor description](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_vertex_attribute_divisor.html).
The standard validation layer did not flag the original invalid combination;
absence of validation errors alone therefore was insufficient evidence.

Mesa `8bb94e2` preserves the KHR/core capability and uses native vertex processing
with firstInstance zero when rebasing is needed. A separate
`zink_instance_rebase.c` shifts each instanced VBO offset by base × stride,
preserves application BaseInstance through a push constant, and restores all
offsets afterward. The existing InstanceID lowering still subtracts the raw
Vulkan base. Direct draws do not map or copy vertex data. The EXT-only path and
KHR/core devices reporting support retain their previous behavior; those device
branches were not exercised on this Mali device.

Indirect draws with a restricted divisor reuse Gallium's argument-buffer
fallback. Mesa `6392c27` consolidates its two decoders, respects padded/zero
stride, bounds map ranges and clamps a GPU count to the caller's maximum.
The argument/count buffers are read synchronously and copied before issuing
direct draws, so this path can stall; GPU-only indirect emulation and performance
acceptance remain open. A separate pipeline-update omission became visible
when split draws changed DrawID: `20260907T124645-8921797d` fails three native
multidraw phases, and `20260907T124701-ce7691b0` fails the indexed phase in
compute mode, each by 128 pixels. Including pending last-vertex-stage key
changes in pipeline/shader-object updates fixes it in `8bb94e2`.

`attribute_draw.c` now checks eight phases, all with the four direct formats
and exact full-image readback. It checks BaseInstance, BaseVertex and DrawID
in the shader, as well as attribute values and default components:

| Phase | Draw | Base / divisor | Additional check |
| --- | --- | --- | --- |
| 0 | arrays, direct | 5 / 1 | Original supported control |
| 1 | arrays, direct | 5 / 2 | Original failing combination |
| 2 | arrays, direct | 0 / 2 | Restored offsets and push data |
| 3 | indexed, direct | 5 / 2 | Index offset 2 bytes, base vertex 7 |
| 4 | arrays, indirect | 5 / 2 | GPU-copied argument buffer, offset 16 |
| 5 | arrays, multi-indirect | 5 / 2 | Two draws, stride 32, DrawID 0/1 |
| 6 | indexed, multi-indirect | 5 / 2 | Index/base-vertex addressing and DrawID |
| 7 | arrays, indirect-count | 5 / 2 | GPU-cleared count 3 clamped to max 2 |

The third command in phase 7 would paint an invalid color over the right half
if executed. First vertex is 7; direct phases use four instances, multidraw
phases use two per draw. Every phase clears blue before drawing. C and host
check all 256 pixels and retain `attributes-0.rgba` through `attributes-7.rgba`.
The runner also requires the expected automatic conversion markers: indexed
cases continue through native processing; eligible decoded indirect arrays can
enter the compute path after argument decoding. This does not add general
indexed vertex-compute conversion.

Final strict pinned-build runs, all with `--vertex-prepass` and standard
Khronos validation including explicitly enabled SyncVal:

| API | Native result | Compute result |
| --- | --- | --- |
| EGL core 3.3 | `20260907T125241-2d09d951` PASS | `20260907T125241-852b1521` PASS |
| EGL compatibility 3.2 | `20260907T125241-38c19fc5` PASS | `20260907T125241-1f6f32b6` PASS |
| GLX core 3.3 | `20260907T125241-ec934de3` PASS | `20260907T125241-6831cd14` PASS |

The pinned Mesa runtime and compiled probe hashes match in all six runs. Each
passes packed inputs, manual compute/deletion, eight attribute phases and three
procedural UBO phases. All 15 retained images match across the six runs.
SyncVal reports no errors. Each native run has 12 SPIR-V modules and each
compute run 85; all 291 validate with Vulkan 1.3 and uniform-buffer-standard-layout.
Per-run disassemblies and `spirv-validation.json` retain tool versions, commands,
module hashes and image hashes. Private Xvfb :184 and its ADB reverse were
removed afterward; the existing Ardesk application was not changed.

This resolves the reproduced valid-buffer first-instance/divisor failure and
DrawID transition failure. It does not validate out-of-bounds/robust fetches,
transform-feedback-count draws, all topology/stage combinations, Vulkan 1.4 or
EXT-only devices. The rebasing helper rejects an instance-buffer base outside
its resource instead of issuing that invalid Vulkan binding. Vertex SSBO limit
remains 0 and Blender startup/rendering still has its separate capability gap.
No GL/Vulkan capability was raised and no full G07/G08/G10/G13 closure is claimed.


## Sampled and unaligned automatic vertex inputs (2026-09-07)

Mesa `080a979` extends the experimental vertex compute path with vertex texture
sampling and byte-aligned VBO fetches. `zink_vertex_pull.c` owns input range
validation, resource sharing and format unpacking, separate from shader replay
and draw state management. Multiple attributes sharing one resource consume
one internal SSBO binding. The decoder supports the admitted plain formats up
to 128 bits with channels up to 32 bits, including RGB8 and RGB16; 64-bit and
fixed-point channels remain excluded.

Fetches combine aligned words only when the attribute needs the following
bytes. For resources ending in a partial word, GPU fill/copy places just their
last one to three bytes into a shared zero-padded tail buffer. No vertex data
is read back to the CPU and no complete VBO staging copy is introduced. Valid
range checks use division before multiplication to avoid address overflow.
The extra tail binding still counts against the reserved binding limit.
Out-of-range input handling and SSBO/VBO aliasing semantics are not validated
by these valid-input cases.

Vertex texture variables and operations remain in the compute shader; implicit
vertex LOD is lowered before the stage change. The replay shader has no texture
resources. Compute sampler states/views and their original counts are restored,
with strong references keeping previous views alive during the temporary
rebinding. Bindless textures, images and general indexed vertex-compute draws
remain excluded. This does not advertise additional GL/Vulkan capabilities.

The existing attribute workload now samples a mipmapped 2D texture at default
LOD and explicit LOD 1, and fetches an integer texture buffer using the instance
record index. All original eight phases remain. Phase 8 adds three attributes
sharing a VBO with stride 21 and byte offsets 1/9/13, including RGB half-float;
the normalized RGB8 instance buffer has stride 5, offset 2 and an exact 45-byte
allocation. Phase 9 uses two half-float components and an exact 206-byte shared
allocation; phase 10 restores the original separate aligned buffers. These
exercise both a partial tail word and an exact end where another word must not
be fetched. In phases 8/9 four attributes use two source bindings plus one tail
binding; runner markers require this converted path, not silent native fallback.

An application compute shader samples texture unit 11 before the sequence and
after every draw, while the VS uses units 3/7. All eleven result vectors must
be `(17,34,51,255)`, alongside the existing 256-pixel stripe check for each phase.
This exercises stage rebinding and restoration of trailing sampler slots; it
is not coverage of every texture target, sampler mode or deletion lifetime.

Strict clean-pinned build, Mali X300, standard Khronos validation with SyncVal:

| API | Native | Compute |
| --- | --- | --- |
| EGL core 3.3 | `20260907T133327-c69cdcc0` PASS | `20260907T133327-cde9bb80` PASS |
| EGL compatibility 3.2 | `20260907T133327-a929f003` PASS | `20260907T133327-82ce899c` PASS |
| GLX core 3.3 | `20260907T133327-ff0531cd` PASS | `20260907T133327-50415656` PASS |

Each result records the same Mesa/runtime/probe manifest. All 18 retained images
match across all six runs, including the packed/manual compute/procedural
regressions. SyncVal is explicitly enabled with no errors. Native runs each
produce 13 SPIR-V modules; compute runs each produce 95. All 324 pass
`spirv-val --target-env vulkan1.3 --uniform-buffer-standard-layout`; per-result
`spirv-validation.json` retains commands, tool version, hashes and disassemblies.
GLX used private Xvfb :185 and its temporary ADB reverse.

Full vertex SSBO support, indexed/indirect compute conversion without CPU
argument decoding, all stages/topologies, caching/performance, desktop window
presentation and Blender remain open. G07/G08/G10/G13 are not closed by this batch.
