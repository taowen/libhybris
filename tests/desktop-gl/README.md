# Desktop OpenGL with product Mesa

The probe uses the parent Ardesk product Mesa build, currently `26.3.0-devel`
at `980c6429e6cb83cb0c394ecad558211f63eab6db` in
[taowen/mesa](https://github.com/taowen/mesa). This fork adds Ardesk WSI to
upstream Mesa; it does not add custom Zink vertex conversion. Zink emits Vulkan
through the standard glibc loader. Select the hybris ICD for an Android vendor
HAL, or the bundled Turnip ICD on an Adreno KGSL device. Mesa implements EGL
and GLX. No custom Zink vertex prepass or Gallium Freedreno KGSL code is built.

The optional `--packed-vertex 1` selects the ICD's experimental static packed
SNORM fallback and its static capability/property policy. On Mali the requested
GL 3.3 context, main draw and twelve packed draws now pass with validation. See the
[actual packed results and limitations](../baseline/packed-vertex.md).

Build prerequisites are the parent Ardesk checkout, Podman and its GL
cross-builder. `build.sh` calls the parent's `tools/build/mesa.sh`, then packages
ELFs directly from `build/mesa-upstream/lib` and compiles the probe using
`tools/ensure-glibc-builder.sh`. There is no separate probe Mesa revision,
Meson configuration or builder override. Product source preparation enforces
its own pin and clean checkout. The manifest records the builder ID, compiler,
product Mesa commit/tree/repository and WSI protocol checksums, probe/source
hashes and packaged ELF hashes. An independent libhybris clone does not bundle
Mesa. Existing result directories retain their original manifests; historical
upstream-only results below do not describe the new product build.

```sh
# Build hybris first when testing an Android vendor HAL.
tools/build-aarch64.sh
tests/desktop-gl/build.sh
python3 tests/desktop-gl/run.py \
  --serial 29854870 --backend turnip --api-version 1.4.359 \
  --profile core33 --vertex-prepass --vertex-draws
python3 tests/desktop-gl/run.py \
  --serial 10AFA31610002QH --hal /vendor/lib64/hw/vulkan.mali.so \
  --api-version 1.3.305 --mali-loader-quirk --profile core32
```

The Mali command currently reports FAIL; it is retained as a negative baseline.
The hybris runner adds missing libraries from the verified baseline runtime to
Mesa's runtime bundle; shared SONAMEs retain Mesa's selected libraries. This
includes the ICD's Wayland dependencies even for a surfaceless EGL run.
On Mali X300, `20260908T145458-d2dddd4d` failed before loading the ICD because
`libwayland-egl.so.1` was absent. With the complete bundle,
`20260908T150211-29217a18` initializes EGL and rejects core 3.3 with
`EGL_BAD_MATCH`; `20260908T150239-1ac3fa90` creates core 3.2 but still fails the
packed vertex cases. These are negative compatibility results, not GL passes.
`--api-version` must match the selected ICD's actual version, not a desired
capability. The build-ID-scoped Mali loader quirk is in libhybris. Current
libraries apply it automatically for the inspected driver; `--mali-loader-quirk`
retains explicit-enable behavior for older library builds. Turnip neither stages nor uses hybris. No GL/GLSL version or
feature advertisement override is set. `--display` selects GLX, with
`LIBGL_KOPPER_DISABLE=true` and the drisw X11 transport; rendering still uses
Zink and the selected GPU Vulkan driver.

The build enables Gallium Zink and Vulkan Turnip (`vulkan-drivers=freedreno`,
`freedreno-kmds=kgsl` are upstream Turnip option names). Ninja caches objects in
`build/mesa-upstream`; this separate directory avoids reuse of the former fork
build. The build reconfigures existing objects, installs into a fresh staging
tree and collects transitive ELF dependencies, including the standard Vulkan
loader. Do not run simultaneous builds in the same output directory.

`--capture-tools` saves the Vulkan capture and a vertex-state index, including
failed GL runs. See [capture usage, device evidence and limits](capture.md).

`--vertex-draws` exercises ordinary GL attribute, indexed, multidraw and resource
cases. `--vertex-prepass` is the existing explicit application compute fixture;
it does not convert arbitrary vertex shaders or enable a private driver mode.
The former `--vertex-execution compute` option has been removed. The original
pixel, GL-error and validation requirements are retained. No unit framework is
used. Mappings must contain Gallium, the standard loader and the selected ICD
(and vendor HAL for hybris). Retained hashes identify deployed files, not
in-memory contents or every dynamic load. The 45-second probe alarm and
60-second host timeout remain; cleanup is restricted to the unique run directory.

## Independent validation logging (2026-09-08)

Validation runs now retain `validation.log` and `validation-result.json`
separately from the application log and render result. `validation_evidence.py`
owns settings and result evaluation. It selects `VK_DBG_LAYER_ACTION_LOG_MSG`
without `VK_DBG_LAYER_ACTION_DEFAULT`, explicitly names the log file and disables
message throttling. This keeps the layer's logger active when the application
registers its own debug messenger. The runner retrieves the file before cleanup,
compares device/host SHA-256, checks the mapped layer and SyncVal activation,
and rejects missing logs or validation errors even when all pixels pass.
A failed render still retains its validation result and tool/settings hashes.

The pinned official Mesa source's `zink_debug_util_callback` calls empty
`zink_error/warn/info/msg` functions in `src/gallium/drivers/zink/zink_screen.c`.
The previous VVL default logger stopped logging once that messenger existed.
The instance startup message therefore proved activation, but subsequent silence
in `probe.log` did **not** prove a clean Vulkan stream. Earlier desktop-GL
claims of zero validation errors, including the historical tables below, must
not be used as validation acceptance unless independently revalidated. Their
pixel and shader-file evidence remains separate. This finding is specific to
the desktop Zink callback path; it does not invalidate independent Vulkan
probe logs that retained their own messages.

All rows below use the existing compiled probe and official Mesa runtime,
lazy descriptors and the unmodified pinned VVL ELF
`ad1587bed5a334930a48dbb04afca1a630ad7da012ea92cb8e17a53ce18f6934`:

| Run | Scope | Rendering | Independent VVL result |
| --- | --- | --- | --- |
| `20260908T193637-328ed899` — Mali | Expanded vertex/compute workload, packed option | FAIL; eight attribute phases | FAIL; four errors, draw/indexed `pNext-09461` |
| `20260908T193638-7b1434d5` — Turnip | Expanded vertex/compute workload | PASS | FAIL; 54 errors for newer, unrecognized pNext structures |
| `20260908T193715-b3043684` — Mali | Main and twelve packed draws | PASS | PASS; SyncVal active, zero errors |

The Mali messages identify divisor two with firstInstance five while
`supportsNonZeroFirstInstance` is false, matching the four direct failing
attribute phases 1/3/9/10 and Vulkan's
[draw restriction](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDraw.html).
The four indirect failures do not receive corresponding CPU-validation
messages; their GPU arguments are not covered by this diagnostic.
The first original-logger run `20260908T193002-ed220a4a` again retained all eight
render failures but no VUID in the application log. Temporary instrumented VVL
runs confirmed the capability was correctly read as false and the divisor chain
was present; the final runs above use the original, uninstrumented installed
layer. This is a logging repair, not a validation-layer or rendering fix.

Turnip messages report structures unknown to the pinned layer's generated
valid-usage data (header 309), including newer properties, device features and
rendering attachments. They remain FAIL; updating the validation dependency
and rerunning is required before declaring a clean stream. No message is
filtered or counted as successful coverage. All three retained logs match
their device hashes. Temporary copies of the passing evidence reject a missing
log, changed bytes, missing activation and missing mapped layer;
`validation-integrity.json` retains those results. Python compilation and diff
checks pass. G06/G12 and application acceptance remain open.

## Validation dependency upgrade to 1.4.362 (2026-09-08)

The source builder now pins VVL `538f91f14cd39274263eb15e6b4228f355370353`
and its matching 1.4.362 dependencies. The SPIRV-Tools iterator fix is retained
in fork `adc7d8b01ae855292822192ae870ab1df19e40a3`; see the
[tool source/provenance record](../../docs/tool-forks.md).
The compiled AArch64 layer SHA-256 is
`6b085cc9058769fde84f88870d64fa106f863f70e32e2d2baf16f87c785fbc7e`.

VVL now reports active features in a `CURRENT-VALIDATION-ENABLED` list rather
than the earlier enum-token startup line. The collector recognizes both exact
activation formats and requires SyncVal in every observed activation block.
It never infers activation from settings or a warning mentioning SyncVal.
The runner uses the existing `validate_sync` setting without the redundant,
now-deprecated `VK_LAYER_ENABLES` environment variable.

| Final run | Rendering | Independent VVL/SyncVal |
| --- | --- | --- |
| `20260908T200122-18e1e819` — Turnip expanded workload | PASS; all fixed images pass | PASS; active SyncVal, zero errors/VUIDs |
| `20260908T200123-67fef8bd` — Mali expanded workload, packed option | FAIL; existing eight attribute phases | FAIL; active SyncVal, the same four direct-draw `09461` errors |

Mesa, hybris and the C probe were unchanged for this tool comparison. The 54
old-layer unknown-structure errors on Turnip are gone without any filtering.
Mali's unsupported nonzero-firstInstance/divisor combination is still reported.
Both logs match their device hashes and retain the new build manifest.
The first new-layer attempts `20260908T195943-b57d18bf` and
`20260908T195944-d50eba7f` remain FAIL because the old activation parser did
not yet understand the new message; their records were not rewritten.
Real old/new log copies also verify that removing activation is rejected, and
a warning merely naming Synchronization does not pass (`activation-integrity.json`).
Python compilation and shell/diff checks passed. This closes the reproduced
outdated-validation-data gap for these fixed desktop cases, not the separate
product-window/teapot gate or G06/G12 in full.

## Product Mesa build reuse (2026-09-08)

The product build was actually reconfigured, compiled and installed through
Ardesk `tools/build/mesa.sh`; the desktop C probe was recompiled. Mesa is
`980c6429e6cb83cb0c394ecad558211f63eab6db`, tree
`1e2f6193016c512f6ff9d15eb019aa798f4c22ac`. Packaged libGL, libEGL, libgallium,
libvulkan, Turnip and zink_dri were byte-compared with the product installation;
all match. The manifest retains their SHA-256s and the three WSI protocol hashes.
The Gallium ELF SHA-256 is
`74a8ec63a3bfdb8f264f96e0f35c07f6d22cb14332bf6e790c56827dfbc65b5b`.

All runs request core33, vertex-prepass and the expanded vertex workload.
VVL is the independently logging 1.4.362 build above. Capture uses the source
build recorded in each result and runs separately from VVL.

| Run | Result |
| --- | --- |
| `20260908T205259-4ea2f51f` — Turnip validation | Rendering PASS; active SyncVal, zero errors/VUIDs |
| `20260908T205258-a1a3645f` — Mali validation, packed option | Rendering FAIL in the same eight attribute phases; four direct-draw `09461` errors |
| `20260908T205343-d4222954` — Mali validation, explicit lazy descriptors | Same rendering and validation failures |
| `20260908T205416-8186e05a` — Turnip capture/replay, preserve compile flags | Capture and replay PASS; 50/50 images match, no diagnostics |
| `20260908T205417-037e5ef1` — Mali capture/replay, lazy descriptors, rebind memory, preserve compile flags | Original rendering FAIL; capture and replay PASS, 27/27 original images match, no diagnostics |

The initial Turnip replay `20260908T205344-2b883153` matched 50/50 images but
correctly remains FAIL because the replay tool warned that it removed pipeline
compile-control flags. The subsequent run explicitly preserves those flags.
No failed records were overwritten. Build-script shell syntax, Python
compilation and diff checks passed; no unit tests were added.

This removes the probe/product Mesa build divergence. These are surfaceless
results: shared Android-buffer WSI presentation, product-window acceptance and
the teapot application gate are not established by these runs. G06/G12 remain
open, including Mali's original attribute failures and validation errors.

## Official upstream results (2026-09-07)

All rows use the pinned upstream commit, standard Khronos validation and
SyncVal. Core 3.3 is requested explicitly; rejection is not retried as success.

| Device/backend | Request | Result | Evidence |
| --- | --- | --- | --- |
| Redmi / Turnip | EGL core 3.3 | `20260907T150334-1f83f981` PASS | GL 4.6; vertex/fragment/compute SSBO 16/16/16; 38 images |
| Redmi / Turnip | GLX core 3.3 | `20260907T150334-a0841785` PASS | Same 38 images; validation output incomplete (see correction above) |
| Mali / hybris | EGL core 3.3 | `20260907T150334-3b67df69` UNSUPPORTED | EGL_BAD_MATCH |
| Mali / hybris | GLX core 3.3 | `20260907T150334-3479d105` FAIL | GLXBadFBConfig |
| Mali / hybris | EGL core 3.2 | `20260907T150505-6d20e1e5` FAIL | GL 3.2 / GLSL 1.50; packed attributes return GL_INVALID_ENUM |
| Mali / hybris | GLX core 3.2 | `20260907T150505-51c67002` FAIL | Same packed attribute failure |

The two Turnip runs have identical full image hashes and 22 SPIR-V modules
each. All 44 modules pass spirv-val with Vulkan 1.3 and uniform buffer standard
layout; per-result JSON retains commands, hashes and disassembly hashes. The
four Mali outcomes are compatibility gaps, not successful desktop GL acceptance.

Installed Blender 4.3.2 on Mali was launched with the official runtime, fresh
HOME/config and no version overrides (`build/blender/upstream-f4b5bd38`). It
shows the OpenGL 4.3-or-higher requirement dialog. Its Python startup marker
never executes; the watchdog terminates the waiting process after recording
its window and mappings (exit 137 is this cleanup, not a spontaneous crash).
Blender was absent from the tested Redmi rootfs; Turnip probe success does not
prove Blender rendering. The user accepts Blender incompatibility through
Zink, so Blender-on-Zink is not an acceptance gate. Private Xvfb :189 and both ADB reverses are removed
after validation.

The parent Ardesk build also compiles official Zink+Turnip, stages the standard
loader and zink/swrast DRI aliases, and builds the APK. Upstream capability gaps
remain open; historical fork passes below are not evidence for this dependency.

## Historical fork results

The records below describe earlier Mesa fork experiments. Their private flags,
commit pins, capability fixes and conversion coverage are historical and are
not part of the current implementation or commands.

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


## Indexed vertex compute and restart replay (2026-09-07)

Mesa `4c60971` extends the experimental prepass to resource-backed 8/16/32-bit
index buffers. The original EBO is an internal read-only SSBO, sharing a binding
if it is also used as a VBO. Range checks cover the index byte span, and the
existing tail buffer handles final partial words. The compute shader obtains
the original index and adds signed base vertex for input addressing and
VertexID; BaseVertex/FirstVertex retain their indexed-draw meaning.
Per-vertex input loads are guarded against exceeding the physical VBO range;
this is not a claim of full GL robustness semantics.

Each compute invocation handles one index occurrence per instance. Instance 0
also writes a uint32 replay index stream after the generated vertex records.
Restart entries become UINT32_MAX and return before executing the original
vertex shader. Other entries select their generated vertex record. Replay keeps
the original topology and restart enable, and uses zero base vertex with an
explicit index/shader-buffer barrier. This private conversion reads no indices
back to the CPU. Client-memory indices, general direct multidraw batches and
indirect argument decoding without CPU access are not newly implemented.

Repeated vertices may execute more than once: GL allows implementation-dependent
vertex invocation reuse/counts, so this workload does not require identical
atomic invocation counts between native and compute execution. Full shader
memory side effects and aliasing still require separate verification.
See [OpenGL 4.3, section 7.12](https://registry.khronos.org/OpenGL/specs/gl/glspec43.compatibility.pdf).

The ordinary application fixture `indexed_draw.c` GPU-copies uploaded indices
into the actual EBO, then draws two triangles. VertexID, signed BaseVertex and
BaseInstance are checked in the VS; fragment PrimitiveID distinguishes the two
triangles and detects incorrect strip continuation. All 256 pixels are checked,
with a blue diagonal avoiding edge-ownership ambiguity. Every case uses a fresh
EBO so allocation reuse does not hide its short tail.

| Phase | Index width | Base vertex | Topology / restart |
| --- | --- | --- | --- |
| 0 | 8 | +4 | Strip, fixed 255, 9-byte EBO |
| 1 | 16 | -2 | Strip, fixed 65535, 18-byte EBO |
| 2 | 32 | +1 | Strip, fixed UINT32_MAX |
| 3–5 | 8 / 16 / 32 | +4 / -2 / +1 | Triangle list with repeated indices |
| 6 | 8 | +4 | All entries restart; retain the blue clear |
| 7 | 16 | -2 | Application restart value 0x1234 |

All draws have a two-element prefix and base instance 3. Phase 7 passes through
Gallium's existing CPU restart rewrite before reaching Zink: the actual prepass
log shows index_size=2 and restart_index=65535. It verifies the combined path,
not GPU-only handling of that application-level custom value. The earlier
`20260907T134758-d6b63643` and `20260907T135052-78f7b919` remain FAIL records:
pixels passed, but the expected tail-binding count was wrong because this
upstream conversion produced a padded index allocation. The revised fixture
separates fixed-restart tail cases from custom-restart compatibility.

Strict pinned-build runs on Mali X300, with standard Khronos validation/SyncVal:

| API | Native | Compute |
| --- | --- | --- |
| EGL core 3.3 | `20260907T135532-c152c3a6` PASS | `20260907T135532-ec193ca0` PASS |
| EGL compatibility 3.2 | `20260907T135532-400bcdbf` PASS | `20260907T135532-10e7ae6f` PASS |
| GLX core 3.3 | `20260907T135532-174351eb` PASS | `20260907T135533-d7f696c1` PASS |

All six record the same clean Mesa/runtime/probe manifest and identical sets of
26 images. Existing packed, UBO, sampled/unaligned attribute and application
compute-state workloads pass. The original direct indexed attribute phase and
two decoded indexed indirect draws now enter the compute path as well; their
markers are present in all three compute runs. Indirect argument/count decoding
still synchronously reads GPU buffers through the previously documented helper.
SyncVal is explicitly enabled and reports no errors. Native runs each have 15
SPIR-V modules, compute runs 125; all 420 validate with Vulkan 1.3 and uniform
buffer standard layout. Per-result JSON records commands, hashes, versions and
disassemblies. GLX uses private Xvfb :186 and a temporary ADB reverse.

No GL/Vulkan capability is raised. All topology/stage combinations, full vertex
SSBO semantics, cache/performance, desktop windows and Blender remain open;
this does not close G07/G08/G10/G13.


## Vertex input descriptor budgets (2026-09-07)

Mesa `aa281c8` assigns internal VBO/index/tail reads to R32_UINT texel buffers
when format, view count and texel range limits allow. Remaining resources use
compact SSBO bindings after the application's SSBO range, below output slot 15.
Both descriptor classes can be used in one draw. Internal inputs can now number
up to PIPE_MAX_SAMPLERS; the SSBO output reservation remains independently 15.
No GL/Vulkan limit is increased. This removes the former unconditional internal
15-buffer bottleneck, not all possible combined resource limits.

Texture instructions use NIR resource dereferences and declared buffer sampler
variables before compiler lowering. Buffer views cover complete words only;
a source shorter than four bytes gets a valid dummy view of its padded tail,
while actual reads use the tail binding. Temporary sampler views are referenced,
bound alongside application views, then restored and released. Index/vertex
bytes remain on the GPU, including the existing partial-tail copy.

`resource_draw.c` queries the actual GL vertex sampler limit (32 on this Mali).
Its six phases exercise 16 VBOs with enough application samplers to leave exactly
16 internal texture slots, all sampler slots occupied with one VBO (SSBO fallback),
return to texel inputs, indexed 16-VBO draws (16 texels + 1 SSBO), a three-byte EBO
(16 texels + 2 SSBOs including tail), and return to arrays. Nine indexed cases now
include a separate three-byte EBO. All original attribute, compute-state, packed
and UBO cases remain. Runner markers require the specific texel/SSBO/mixed paths.

The development record `20260907T141152-db1a324f` retains the missing NIR texture
dereference crash, fixed before final validation. `20260907T141457-4549c26f`
retains a verifier failure: pixels passed, but 16 samplers did not exhaust the
actual 32-slot budget. The fixture now queries the limit instead of assuming it.
Also, GL texture units are mapped to stage-local sampler slots: the earlier
unit-11 compute fixture verifies state restoration, not a literal driver slot 11.

Strict clean-pinned builds, Mali X300, standard validation with SyncVal:

| Mode | API | Result | SPIR-V modules |
| --- | --- | --- | --- |
| native | core | `20260907T142408-0df65dc9` PASS | 19 |
| native | compat | `20260907T142408-04689fee` PASS | 19 |
| native | glx | `20260907T142408-056081cc` PASS | 19 |
| compute | core | `20260907T142408-bb383637` PASS | 146 |
| compute | compat | `20260907T142408-61261fba` PASS | 146 |
| compute | glx | `20260907T142408-1f9a17a5` PASS | 146 |

All six manifests match the final pinned build; their 33 retained images match
byte-for-byte. SyncVal is explicitly enabled and reports no errors. All 495
SPIR-V modules pass Vulkan 1.3 validation with uniform buffer standard layout;
per-result JSON retains commands, hashes, tool versions and disassemblies.
Private Xvfb :187 and its ADB reverse were removed after GLX validation.

This is still an experimental prepass. Full application vertex SSBO semantics,
resource aliasing, robustness, all stage/draw forms, cache/performance and Blender
remain open. Custom restart and indirect argument CPU rewrites are unchanged.


## Direct multidraw vertex conversion (2026-09-07)

Mesa `597b753` decomposes supported direct multidraw calls through Gallium's
existing draw helper and re-enters the prepass for each nonempty subdraw. This
preserves DrawID advancement across zero-count draws and restores compute and
vertex state before the next subdraw, including local native fallback when a
subdraw cannot be converted. Single-draw dispatch limits are checked per subdraw.
Restart metadata is read only when restart is enabled. No capability is raised.

The new ordinary GL fixture uses glMultiDrawArrays and
glMultiDrawElementsBaseVertex with 8/16/32-bit GPU-copied index buffers,
nonzero index offsets, base vertices +3/-2/+8, and an empty second subdraw.
Each vertex checks its vertex range, base vertex, instance and DrawID. The
fragment shader draws red/green/blue bands for DrawIDs 0/2/3. A subsequent
single draw checks DrawID reset and produces red/black bands. The host checks
all five full images and separately requires converted DrawIDs 2 and 3 for
all four multidraw variants; a native fallback cannot satisfy those markers.

Development result `20260907T143316-e498e39e` had all pixels passing but failed
the old aggregate input count check. The verifier now accounts for the three
additional sub-word indexed conversions and checks the new DrawID markers.
Native development result `20260907T143315-fc9e567a` passed. These results precede
the final deterministic disabled-restart diagnostic and are not the final pin.

Final clean-pinned build, Mali X300, standard validation and SyncVal:

| Mode | API | Result | SPIR-V modules |
| --- | --- | --- | --- |
| compute | compat | `20260907T143434-e9721755` PASS | 185 |
| compute | core | `20260907T143434-31b9cbc2` PASS | 185 |
| compute | glx | `20260907T143434-83f50542` PASS | 185 |
| native | compat | `20260907T143434-97c8a52c` PASS | 22 |
| native | core | `20260907T143434-98e45bc7` PASS | 22 |
| native | glx | `20260907T143434-c7362d85` PASS | 22 |

All six results match the final build manifest and contain 38 byte-identical
images. All 621 dumped SPIR-V modules pass spirv-val with Vulkan 1.3 and uniform
buffer standard layout; per-result spirv-validation.json retains commands,
hashes, versions and disassembly hashes. SyncVal reports no errors. This covers
direct multidraw on this Mali driver, not GPU indirect argument conversion,
custom restart without CPU rewriting, every fallback combination, all stage
forms, resource aliasing/robustness, performance or Blender. Vertex SSBO remains
unadvertised. Private Xvfb :188 and its ADB reverse are removed after the runs.
