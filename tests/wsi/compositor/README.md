# External compositor prerequisite

libhybris does not build, install or package a compositor APK or Xwayland.
The previous JNI/Java APK host and builder have been removed from this tree.
Their historical implementation remains available in git history.

Install and start Arlinux (`io.taowen.arlinux.debian`) using Arlinux build tooling.
The default X display is `:1`. An explicitly selected external debuggable APK
may provide the same service contract. It
must provide a Wayland endpoint for Wayland checks. The runner defaults to
`files/runtime/wayland-0`; `--runtime-dir` and `--wayland-display` select the
actual endpoint published by the APK. X11-only checks use DISPLAY directly.
For X11 checks its compositor/desktop must also start a local X display with
TAWC-DRI 0.3. Xwayland and its patches belong to Arlinux or that external APK;
android_wlegl belongs to anlabwc. Configure X authentication there as needed.

The [client runner](../README.md) uses `run-as` only to stage and execute probe
clients under the selected package UID. It records the installed APK hash and
compositor PID/start time, and leaves the external service running. It does
not verify or prescribe the APK's internal server bundle or build layout.
