# One source branch, two libc builds

`arlinux` is the maintained integration branch for both Linux and Bionic Wine.
Both consumers pin the same libhybris commit. The branch contains the shared
Vulkan ICD, compatibility layer, BC kernels and graphics protocols; fixes are
made here once, not carried on separate libc-specific source branches.

| Product | Build mode | Owner of the build graph and operating guide |
| --- | --- | --- |
| `linux-hybris` | ARM64 glibc on the standard ARM64 runner | [arlinux-rootfs](https://github.com/taowen/arlinux-rootfs/blob/main/docs/BUILD.md) |
| `wine-vulkan` | ARM64 Bionic cross-compilation with the Android NDK on the standard x64 runner | [Wine](https://github.com/taowen/wine/blob/arlinux/arlinux/BUILD.md) |

The workstation entry for both is the Android repository's
[build.ps1](https://github.com/taowen/arlinux/blob/main/docs/BUILD.md).
The libc bridge and Linux build adapters stay separate from Android-native
loading, while the compatibility-layer source list and kernels are shared.
Each product retains its independent workflow dispatch, compiler cache and
artifact identity. A common source commit does not imply interchangeable
binaries or allow one product to consume another product's cache.

The two workflow wrappers in this branch are generated from their owners'
canonical templates. They only dispatch the selected pinned framework recipe;
they do not implement another build graph. Keep the rootfs and Wine submodule
pins synchronized when integrating a graphics change. Update both pins and
validate each product before promoting its public release.

## Standalone Android diagnostics

Every Bionic Vulkan build also produces `diagnostics/libhybris-diagnostics.apk`
and its JSON identity. The APK embeds the exact compiled compatibility layer,
its source commit, library hashes and licenses. The component publisher exposes
both files in the same `arlinux-wine-vulkan-debug` release. From the Android
workstation's sole entry:

```powershell
.\build.ps1 hybris-diagnostics -WineRef FULL_PUSHED_WINE_COMMIT
```

Install the returned APK as `org.arlinux.hybrisdiagnostics`; no ARLinux, Linux
instance, Wine, root or system GPU-layer settings are needed. It uses Android's
Vulkan loader and driver with the standard layer negotiation/link interfaces.
The normal production WSI adapter is not used: these are offscreen GPU experiments,
not window/swapchain acceptance. Each experiment executes the APK's native probe
as a fresh child process and checks BC upload, sampled pixels, original compressed readback and
restoration of the app's compute bindings. A preview and report remain in the app.

The home screen opens independent probe pages. Each page owns its controls,
native executable and result directory; reports from different probes remain
available when switching pages. Vulkan loading/resource lifetime and Android
process cancellation, identity verification and report display are shared.

| Probe | Activity | Native executable | Result directory |
| --- | --- | --- | --- |
| BC textures | `.BcProbeActivity` | `libhybris_bc_probe.so` | `files/bc/` |
| Robustness2 | `.RobustnessProbeActivity` | `libhybris_robustness_probe.so` | `files/robustness/` |
| Specialization constants | `.SpecConstantsProbeActivity` | `libhybris_spec_constants_probe.so` | `files/spec_constants/` |
| Multi-viewport | `.MultiViewportProbeActivity` | `libhybris_multiviewport_probe.so` | `files/multiviewport/` |
| Indirect multi-draw | `.MultiDrawProbeActivity` | `libhybris_multidraw_probe.so` | `files/multidraw/` |
| Dual-source blend | `.DualSourceProbeActivity` | `libhybris_dual_source_probe.so` | `files/dual_source/` |
| Polygon modes | `.PolygonProbeActivity` | `libhybris_polygon_probe.so` | `files/polygon/` |

For device automation, start the relevant Activity directly with a `mode` extra.
BC accepts `native`, `normal`, `decode`, `auto`, `fast`, `default` or `high`, and
`size=64` or `1024`. Robustness2 accepts `robust-native`, `robust`, `robust-force`
or `robust-off`; it has no texture-size option. For example:

```sh
adb -s SERIAL shell am start -n org.arlinux.hybrisdiagnostics/.BcProbeActivity --es mode normal --ei size 64
adb -s SERIAL shell am start -n org.arlinux.hybrisdiagnostics/.RobustnessProbeActivity --es mode robust-force
```

Read `report.txt` and `trace.txt` in each result directory using ADB `run-as`.
BC also saves `preview.png`; robustness2 does not produce a texture preview.
The APK identity records a separate executable name and SHA-256 for each probe,
and each run verifies both its executable and the shared compatibility layer.
Back cancels the running child and returns its page to the controls; Back from
an idle page returns to the home screen. No instrumentation package is installed.

The native executable runs under the app's ordinary UID. Unlike the zygote's
inherited secure-execution state, `exec` supplies the child's actual security
state, so diagnostic environment controls work without relaxing the layer's
`AT_SECURE` guard. Cancellation and timeout terminate only that child.

The SDK 34/build-tools 36.0.0 packaging step runs on the same Linux x64 runner as
the NDK build and does not request Wine compilation. It uses a disposable diagnostic
signing key, never the product signing key. A new build may require uninstalling
the previous diagnostic APK; its private reports are then removed. Retain useful
evidence outside the app first. Keep device records outside Git. Real application
and game regression remains required after these targeted experiments.

## Robustness2 compatibility

Native robustness2 feature bits are retained. On drivers lacking all or part of
robustness2, the layer follows MaliBridge's compatibility policy: enable core
buffer/image robustness where available, and replace null descriptors with
initialized read-only resources or separate writable sinks. This is an
approximation: writable null resources can read back prior writes, and core
buffer robustness does not promise robustness2's precise zero/out-of-bounds
semantics. It must not be described as Vulkan conformance.

`HYBRIS_ROBUSTNESS2=0` disables this compatibility policy; `force` exercises it on
a native-capable GPU for diagnosis. The default is automatic per missing feature,
not an application-specific rule. Full null compatibility hides descriptor-buffer,
shader-object and ray-tracing extensions, whose resource paths are not covered.
Image stand-ins infer view shape and numeric class from pipeline SPIR-V; unknown
or conflicting declarations use a 2D fallback. Multisampled and 64-bit typed null
images are not supported by that approximation.

The diagnostic APK accepts `robust-native`, `robust`, `robust-force` and
`robust-off` modes. It dispatches real GPU reads, writes and atomics against null
and out-of-bounds descriptors, checks normal/template updates leave caller-owned
memory intact, and reports compatibility limitations separately from failures.
These focused probes do not replace real application acceptance.

## Mali specialization-composite normalization

The compatibility layer follows MaliBridge's `server_spvfix.c`: an
`OpConstantComposite` whose constituents depend on specialization constants is
normalized to `OpSpecConstantComposite`, including transitive composite
references. Ordinary constant composites and `VkSpecializationInfo` are retained.
The pass operates on the layer's private captured code, before other shader
rewrites and backend module creation. Graphics and compute inline modules use
the same module path; inline chains currently require the module node first,
consistent with the existing graphics path. Unsupported chain layouts fail
explicitly instead of mutating caller-owned structures.

`HYBRIS_MALI_SPEC_CONSTANTS` defaults to Arm vendor GPUs, `0` disables the pass,
and `force` selects it on other GPUs for diagnosis. Secure execution disables
this policy. This repairs a historical application SPIR-V form; it is not a
claim that every affected module is valid under the current SPIR-V rules or
that all Mali drivers miscompile valid specialization constants.

The independent specialization probe accepts `spec-native`, `spec-normal`,
`spec-force` and `spec-off` modes. It uses a build-time validated reference and
a deliberate historical composite variant only on an enabled repair route.
Native/disabled routes run references, never the historical variant. Checks
sample an actual texture with defaults, partial/full overrides, specialization
operations and ordinary constant controls; reuse one module across specializations;
and verify caller code remains unchanged. Inline compute is checked when native
maintenance5 is available. A repair trace confirms that force mode executed the
pass. These are targeted GPU checks, not application/game acceptance.

## Multi-viewport compatibility

The MaliBridge-derived fallback advertises `multiViewport` and at least 16
viewports when the native driver lacks the feature. It filters that feature
out of device creation and keeps only viewport/scissor index zero in static
pipelines, dynamic setters and core/EXT with-count setters. Updates starting
at a nonzero index are discarded, with bounded diagnostic warnings.
Native-supporting devices pass through unless `HYBRIS_MULTI_VIEWPORT=force`
is explicitly selected. `HYBRIS_MULTI_VIEWPORT=0` disables the approximation.

This is not a conformant multiViewport implementation: additional viewports,
scissors and shader `ViewportIndex` are not emulated. Applications that actually
render to multiple viewports may render incorrectly. This policy has no
application-name matching and is shared by glibc and Bionic builds.

The diagnostics APK has a separate Multi-viewport Activity and native probe.
`viewport-native`, `viewport-normal`, `viewport-force` and `viewport-off`
compare offscreen graphics readback and caller-state preservation using legacy
and Features2 device creation, GDPA/GIPA, static/dynamic state and available
core/EXT with-count commands. The probe renders only viewport zero; a passing
result proves that bounded compatibility path, not full multiViewport or game
acceptance. Use the standalone diagnostics entry above.

## Indirect multi-draw compatibility

When native `multiDrawIndirect` is missing, the MaliBridge-derived fallback
exposes it and `maxDrawIndirectCount=UINT32_MAX`, filters the emulated feature
from private device-create copies and splits `vkCmdDrawIndirect` /
`vkCmdDrawIndexedIndirect` into one native draw per command-buffer entry.
Offsets advance by the caller's stride; the indirect buffer is never read or
rewritten on the CPU. Existing upload visibility hooks run before splitting.
Zero/one draws pass through. Only logical devices requesting the emulated
feature use the splitter; native-supported devices retain their native calls.
`HYBRIS_MULTI_DRAW_INDIRECT=0` disables this policy; `force` exercises it for
diagnosis. It is shared by Bionic and glibc, without app-name matching.

This is a compatibility approximation: splitting resets shader `DrawIndex` to
zero for each draw. Shaders depending on the original multi-draw index are not
emulated and can render incorrectly. GPU-counted indirect draw commands are
not synthesized or advertised. This does not imply Vulkan conformance.

The separate diagnostics Activity/native probe accepts `indirect-native`,
`indirect-normal`, `indirect-force`, `indirect-off` and
`indirect-unrequested`. It transfers indirect commands to the GPU, uses indexed
and non-indexed draws, packed/padded strides and zero/one/three draws, then
compares exact pixels and untouched command-buffer contents. It checks legacy
and Features2 device creation, both GIPA/GDPA, and reports the DrawIndex
limitation through a real shader. When the feature is unavailable/disabled,
three draws use an explicit application-side single-draw reference. Those
controls must not be reported as native multiDrawIndirect support. These
offscreen checks do not replace application or game acceptance.

## Dual-source blend approximation

When native `dualSrcBlend` is absent, the MaliBridge-derived policy exposes
the feature and a minimum `maxFragmentDualSrcAttachments` of one. It clears
the emulated feature in private device-create inputs, substitutes SRC0 factors
for SRC1 in copied static blend state and relocates fragment Index=1 outputs
to unused output locations with Index=0. Captured application modules, blend
state and specialization inputs remain unchanged. Native-supported devices
retain native behavior. `HYBRIS_DUAL_SOURCE_BLEND=0` disables the policy;
`force` selects it for diagnosis. Only devices requesting the feature rewrite
pipelines. Bionic and glibc share the source, without application-name rules.

This approximation changes blending when source zero and source one differ.
It is not full dual-source blending or Vulkan conformance. PanVK can implement
actual source-one blending through its internal blend shaders and Mali compiler;
this layer above the proprietary driver cannot invoke that driver-internal path.

The local rewrite supports ordinary scalar/vector 32-bit fragment outputs
with direct decorations after the existing entry-point normalizer. Arrays,
interface blocks, unresolved decoration groups and exhausted output locations
fail explicitly rather than overlapping another output. The existing pipeline
conversion restrictions also apply: shader objects and graphics pipeline
libraries are unavailable. Dynamic color-blend equations are disabled when
this approximation is active; static blend state is the supported path.

The separate probe accepts `dual-native`, `dual-normal`, `dual-force`,
`dual-off` and `dual-unrequested`. It compares real offscreen blended pixels
for all four SRC1 factor variants in source/destination color and alpha slots,
checks blending disabled and preservation of an existing auxiliary output,
checks explicit rejection of unsupported interfaces, and verifies unchanged
caller inputs. Legacy/Features2 device creation and GDPA/GIPA pipeline queries
are covered. Native/disabled/unrequested modes use legal first-source controls
when native dualSrcBlend is absent. These results do not establish game or
application acceptance.

## Polygon LINE/POINT fallback

`HYBRIS_POLYGON_MODE` defaults to devices missing `fillModeNonSolid`; `0`
disables it and `force` selects it for diagnosis. The feature is filtered from
private device-create inputs without enabling unrelated indirect-draw features.
A non-solid pipeline keeps its filled application handle and a line-list or
point-list variant using the same shaders, vertex input and application layout.
Non-indexed LINE draws use host-generated triangle-edge indices; indexed LINE
draws use MaliBridge's GPU index-expansion kernel. POINT draws use the same
vertices/indices with point-list topology. Application shaders must meet the
ordinary point-size requirements. Original pipeline, index binding and dynamic
topology/restart state are restored after each variant draw.

Index buffers receive storage usage and word padding at creation for GPU index
reads. Only indices are processed; there is no vertex-shader compute rewrite,
descriptor-layout extension or post-VS replay. Private prepass pools and scratch
resources live with the existing command recording. Both ordinary render passes
and secondary render-pass command buffers use this path. Submission inserts
index prepasses immediately before each owning primary and extends semaphore
wait stages to cover compute. Legacy device-group masks and Submit2 command
metadata are preserved. Unknown prefix nodes before a legacy device-group
submit node are rejected when they cannot be copied safely.

This is approximate polygon-mode support, following MaliBridge. Triangle
culling, polygon depth bias, geometry/tessellation/mesh shaders, indirect draws
and dynamic polygon mode are not emulated. Shared strip/fan vertices can be
processed once rather than once per triangle in POINT mode. Unsupported draws
use the full filled draw and log a bounded warning. Indexed LINE also uses
filled fallback for simultaneous-use recordings, index writes/dispatches
earlier in that recording, fan/list restart and negative vertex offsets with
strip restart. If a primary writes a secondary's index-prepass input before
executing it, EndCommandBuffer fails explicitly instead of submitting stale
edges. Shader writes to index memory through aliasing/resources not identified
by these command-level guards remain outside this compatibility path.

The independent probe accepts `polygon-native`, `polygon-normal`,
`polygon-force` and `polygon-off`. It compares exact native-reference pixels
for triangle lists/strips/fans, LINE/POINT, indexed/non-indexed, 16/32-bit indices,
secondary command buffers, descriptor/push-constant state and GDPA/GIPA. It also
checks same-submission uploads, index-binding restoration, strip restart and
explicit primary/secondary write-hazard rejection. Native/disabled modes use
legal explicit line/point references on GPUs missing fillModeNonSolid. These
checks do not establish native polygon-mode conformance or game acceptance.
