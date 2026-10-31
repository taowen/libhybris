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
operations and nested composites; reuse one module across specializations;
and verify caller code remains unchanged. Inline compute is checked when native
maintenance5 is available. A repair trace confirms that force mode executed the
pass. These are targeted GPU checks, not application/game acceptance.
