# Disposable window-test APK

The current [build and run instructions](../README.md) use one APK containing
anlabwc, TAWC-DRI Xwayland, their Android dependencies and xkb assets. Package
`io.taowen.hybriswsitest` owns its Activity Surface, files and Wayland socket.
The default runner always uses this package; no Ardesk process or rootfs is
started. The old `compositor/run.py` wrapper has been removed.

`build.py --backend-apk FILE [--backend-library FILE] --x11-build DIRECTORY`
compiles the small JNI/Java host, imports the backend dependency closure and
xkb assets, and verifies/packages every server ELF from the X11 build manifest.
The optional backend replacement changes libanlabwc while keeping the input
APK's support libraries. The Activity extracts the bundled Xwayland before
starting anlabwc. Xwayland is launched on demand by the selected X11 probe's
native supervisor using a private inherited socket.

The build defaults are SDK platform android-35, build-tools 36.0.0 and NDK
29.0.14206865. Host dependencies include javac, keytool, patchelf and the SDK.
Output, signing key and manifest stay under ignored `tests/wsi/build/compositor`.
Target SDK 28 enables the development `run-as` executable workflow; this is a
disposable test fixture. Imported backend binaries are hashed dependencies,
not a claim of source reproducibility. The included Xwayland manifest records
its source revision, patch and dependency hashes.
