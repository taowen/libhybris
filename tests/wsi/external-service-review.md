# External window-service boundary (2026-09-08)

libhybris is the glibc/bionic driver bridge and a WSI protocol client. Xwayland
and TAWC-DRI server patches belong to Ardesk's third_party/xwayland or an
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
