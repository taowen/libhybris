# Desktop OpenGL through Zink and hybris

This builds an actual desktop GL frontend: source-built Mesa/Zink → standard
glibc Vulkan loader → hybris ICD → Android vendor Vulkan HAL. EGL is Mesa's
implementation, not the hybris GLES EGL frontend. The initial integration uses
surfaceless EGL pbuffers, with no X server, APK or compositor. It does not yet
provide an application launcher, GLX or displayed desktop GL windows.

Build prerequisites are the parent Ardesk checkout's clean Mesa revision
`1cb7f0a1c9a5438045f89ad4aa83eda8fbafa09e`, its AArch64 cross file, Podman and
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

Actual results on 2026-09-07, Mesa 26.3.0-devel at the revision above:

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

The fixed Mesa source checks only EXT_vertex_attribute_divisor for instanced
vertex attributes. The recorded Mali capability list advertises its KHR name
but not EXT. This is one concrete blocker for GL 3.3, not a proof that changing
one extension check will satisfy all higher GL versions. The Adreno baseline
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
