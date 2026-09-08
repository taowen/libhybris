# XCB/Xlib client probes

Build the client and Android watchdog with `python3 tests/x11/build.py`.
The only optional input is `--ndk`; the glibc client uses the pinned baseline
cross-builder. `build/manifest.json` records source, compiler, builder and the
two client executable hashes, plus the client's transitive ELF runtime closure.
The existing `tools/stage-runtime.py` collects that closure with the same library
search order as the hybris builder. Staging verifies every hash and rejects
conflicting shared libraries. X11 clients no longer rely on a removed EGL
plugin to bring in `libX11`. No X server is built or packaged here.

Run through the [window integration runner](../wsi/README.md) against an
already running compositor APK and its existing local `DISPLAY`. Both XCB and
Xlib use that display and optional `XAUTHORITY`. The watchdog owns only its
client process; it neither creates a listening socket nor launches a server.

Xwayland and TAWC-DRI patches belong to Ardesk's `third_party/xwayland` or an
external test APK; android_wlegl belongs to anlabwc. The libhybris ICD owns
only the TAWC-DRI/android_wlegl client. It requires a local AF_UNIX connection,
a supported TrueColor visual and TAWC-DRI 0.3. PresentBuffer sends gralloc
handles through SCM_RIGHTS; BufferRelease controls buffer reuse. Xlib shares
its XCB connection without changing application event ownership. There is no
PRESENT_SOCKET fallback. GPU release fences currently require a host wait.

The old server builder and vendored TAWC patch have been removed. Historical
results in the [integration review](../wsi/integration-review.md) and
[history archive](../wsi/history.md) describe the previous fixture and do not
prove the externally managed service workflow. Missing-protocol checks now
require an existing display without TAWC-DRI, supplied by its external owner.
