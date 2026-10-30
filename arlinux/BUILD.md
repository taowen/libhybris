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

The native route, normal compatibility policy, forced decode and forced transcode
quality modes are separate buttons. The large-texture option exercises automatic
fast quality at 1024x1024. `mode` (`native`, `normal`, `decode`, `auto`, `fast`,
`default`, `high`) and `size` (`64`, `1024`) are also accepted as MainActivity intent
extras for device automation. Reports and layer traces can be read through ADB
`run-as` from `files/report.txt` and `files/trace.txt`. The app can share its report.
No test instrumentation package is installed.

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
