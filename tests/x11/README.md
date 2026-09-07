# X11 probe and Xwayland build inputs

These sources are the XCB/Xlib part of the single
[window integration runner](../wsi/README.md). The test APK contains anlabwc and
the default TAWC-DRI Xwayland. glibc clients and libhybris are deployed separately.
Use the main README for the one build/install/run workflow and supported cases.

## Xwayland builder

```sh
python3 tests/x11/build.py \
  --xwayland-source /path/to/ardesk/third_party/xwayland \
  --ndk-prefix /path/to/ardesk/build/ndk-prefix
```

The tested Android-patched Xwayland base is 24.1.6, commit
`6372702c0c12461787d9fab351912b9663dbcea4`. The supplied dependency prefix must
contain `android-cross.ini`, `native.ini`, headers, libraries and pkg-config
files. The builder archives the source revision into an isolated ignored copy,
applies the protocol patch there and never installs into that prefix.
It uses Meson/Ninja, Git, tar, patchelf, Android NDK 29.0.14206865 and the baseline
pinned cross-builder. GLX, glamor, DRM, DRI3 and MIT-SHM are disabled; this fixture
exercises gralloc buffer transport. No Mesa changes are needed.

`build/manifest.json` records the Xwayland revision, patch, dependency/compiler
and ELF hashes. Pass `--x11-build tests/x11/build` to the APK builder to include
that verified server bundle. Changing this server requires rebuilding the APK,
or an explicit `--server-binary` override; normal runs do not silently deploy
the host's Xwayland. The glibc client and native supervisor remain independent
build outputs, allowing client/library iterations without reinstalling the APK.

## Protocol and ownership

The ICD requires a local AF_UNIX connection, a supported TrueColor visual and
TAWC-DRI 0.3. PresentBuffer sends gralloc handles with SCM_RIGHTS. BufferRelease
arrives on a private XCB special-event queue and controls reuse. Pending
presentations and imported Vulkan images retain their native-buffer references.
Xlib shares its XCB connection without changing application event ownership.
There is no PRESENT_SOCKET fallback. GPU release fences are consumed with a
host wait before submission, matching the existing android_wlegl limitation.
Protocol trace output is bounded to 64 records per owner.

`patches/tawc-dri.patch` is copied without modification from
[wmww/tawc](https://github.com/wmww/tawc), commit
`4d74c2db0c9cc927118d493eba32107d7e8971d5`, file
`deps/xwayland-patches/xwayland/02-tawc-step3-ahb-present.patch`.
Its copyright and MIT license notices are retained. Patch SHA256:
`eba1d87a41943dedb958c27e660ce1f7e1e05cd47d222fde32d366995bc61a46`.

The [integration review](../wsi/integration-review.md) records Redmi/Mali
presentation, resize/out-of-date, protocol rejection and timeout results,
including earlier failed screenshots. The [history archive](../wsi/history.md)
retains the superseded standalone-runner evidence. Rootless/multiwindow,
minimize, disconnect recovery, long-running FD accounting, X11 capture and CTS
remain open; the replacement-libvulkan frontend has no X11 WSI in this batch.
