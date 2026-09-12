# External window-service boundary (2026-09-08)

libhybris is the glibc/bionic driver bridge and a WSI protocol client. Xwayland
and TAWC-DRI server patches belong to Arlinux's third_party/xwayland or an
external test APK; android_wlegl belongs to anlabwc.

Removed from this tree: the Xwayland source archive/patch/build/dependency
bundle steps, vendored server patch, compositor APK builder, JNI/Java host and
APK manifest. The X11 watchdog no longer binds/listens on sockets or launches,
reaps or kills an X server. No server override is accepted. Existing ignored
build caches are not inputs to the new client-only manifest or deployment.

The runner attaches to an installed, already running debuggable package. It
never installs an APK, unlinks the compositor socket, restarts the package or
stops it after the test. Package PID/start time and installed APK hash remain
recorded. It cleans up only its own client deployment and processes. XCB and
Xlib both connect through DISPLAY, with optional device-side XAUTHORITY. The
existing protocol version, presentation, release and pixel checks remain.
A missing display is a real failure, not an instruction to build a server.

Validation:

- `python3 tests/x11/build.py` built the native watchdog and glibc XCB/Xlib client
  in the pinned cross-builder, with no warnings in the final build. Its manifest
  contains only `probe-xcb` and `x11-session`; no server or APK artifacts.
- Python compilation and runner help passed; old server build/override options
  are absent. No new unit-test harness was added.
- Redmi `20260908T131027-fd561301`: XCB control attached to the existing APK but
  could not connect to external `:0`; exit 2, FAIL. The compositor identity was
  retained and cleanup errors were empty. This used the client before the final
  explicit connection-error message was added.
- Mali `20260908T131227-85c572a8`: final Xlib control logged `X11_CONNECT failed
  display=:0 api=xlib`, exit 2, FAIL. The runner launched no replacement server.
- Redmi `20260908T131105-909b1d0d`: attachment failed because the external
  compositor was no longer running. The runner did not restart it.
- After starting the same installed APK through its Activity, Redmi
  `20260908T131212-f74ec8cd` passed Wayland present: 24 exact readbacks across
  three sizes and six screenshot checks. Package PID/start time stayed stable,
  cleanup errors were empty, and the runner left the external service running.

The APK was already installed; no APK or Xwayland was built or installed during
these checks. Production library code is unchanged in this ownership change.
The current installed APK does not provide a connectable external :0 for these
X11 checks. Successful TAWC-DRI presentation against an externally managed
server remains to be rerun once its owner provides that display. Historical
private-server passes are retained as history, not claimed for this workflow.


The old installed test APK explicitly set `WLR_XWAYLAND=/system/bin/false`:
its compositor never owned an enabled Xwayland service, because the former
probe supervisor supplied it. Its mere presence is therefore insufficient for
new X11 tests. Arlinux already supplies `libxwayland.so` via WLR_XWAYLAND and
anlabwc sets DISPLAY from the Xwayland instance it creates. Enabling that
existing owner-side chain and exposing its actual endpoint resolves the missing
service prerequisite; the local display number must not be guessed.

The client runner now accepts explicit `--runtime-dir` and `--wayland-display`
(including an absolute socket path), so an external APK need not reproduce the
old fixture's directory layout. X11-only attachment checks the package identity
without imposing a Wayland socket location; its X client still verifies the
actual display connection and protocol. No new anlabwc test service or control
protocol is introduced.

Endpoint follow-up validation: the Wayland probe rebuilt successfully using
`tests/wsi/build.sh`. Mali `20260908T131733-abc11dc3` passed 24 readbacks and
six screenshots with explicit runtime directory and an absolute Wayland socket
path. Mali XCB control `20260908T131824-d682e188`, with a deliberately nonexistent
Wayland runtime directory, reached the X connection and failed there with
`api=xcb error=1` for :0. It did not reject an irrelevant Wayland endpoint or
launch a server. Both runs left the observed compositor running.
