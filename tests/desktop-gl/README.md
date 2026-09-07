# Desktop OpenGL through Zink and hybris

This builds an actual desktop GL frontend: source-built Mesa/Zink → standard
glibc Vulkan loader → hybris ICD → Android vendor Vulkan HAL. EGL is Mesa's
implementation, not the hybris GLES EGL frontend. The initial integration uses
surfaceless EGL pbuffers, with no X server, APK or compositor. It does not yet
provide an application launcher, GLX or displayed desktop GL windows.

Build prerequisites are the parent Ardesk checkout's clean Mesa revision
`37f170c789f75792c209fa9221f7ab67bbd5b87f`, its AArch64 cross file, Podman and
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
are vertex attributes, textures/images, bindless and subgroup operations,
clip/cull arrays, transform feedback, indexed/indirect/multidraw, geometry and
tessellation stages, active queries and draws exceeding dispatch/storage limits.
Native execution remains in use for excluded draws. Programs and output storage
are created per draw; general caching, complete lowering, performance work and
application vertex SSBO acceptance remain open. No Blender startup/rendering
pass, full GL conformance, other-device conversion or complete G08/G10 closure
is claimed.
