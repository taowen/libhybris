# Disposable Android compositor

This small debug APK runs the existing anlabwc backend on its own Activity
Surface. Its package is `io.taowen.hybriswsitest`, with its own UID, process,
files/runtime directory and Wayland socket. It does not start Ardesk, a rootfs,
xterm, Gladio or Vortek. It is a developer test fixture, not a desktop app.

Build from an explicitly supplied APK containing libanlabwc, its native
libraries and xkb assets:

```sh
python3 tests/wsi/compositor/build.py --backend-apk /path/to/ardesk-debug.apk \
    --sdk /path/to/Android/Sdk
/path/to/ardesk/tools/install-apk.sh --serial SERIAL \
    tests/wsi/build/compositor/hybris-wsi-test.apk
tests/wsi/build.sh
python3 tests/wsi/compositor/run.py --serial SERIAL
# Keep the compositor alive across ten sequential client processes:
python3 tests/wsi/compositor/run.py --serial SERIAL --repeat 10
```

The build compiles the small JNI host and Java Activity with the SDK/NDK,
selects the backend's transitive DT_NEEDED dependencies from the supplied APK,
packages xkb, aligns and signs the result with a local test key. The default
NDK is 29.0.14206865, build-tools 36.0.0, compile platform android-35. It also
needs Python, javac, keytool and patchelf. Build output and the signing key stay
under ignored tests/wsi/build/compositor. manifest.json records backend/APK,
source and native-library hashes and tool versions. The backend is imported
binary code: this does not establish backend source reproducibility. Review
its provenance before choosing another APK. Target SDK 28 supports this
run-as native executable development workflow; this is not a production APK.

To test a source change in anlabwc while retaining the selected APK's support
libraries and xkb data, build that checkout and replace just its backend:

```sh
meson compile -C /path/to/ardesk/build/ndk-anlabwc
python3 tests/wsi/compositor/build.py --backend-apk /path/to/ardesk-debug.apk \
    --backend-library /path/to/ardesk/build/ndk-anlabwc/libanlabwc.so \
    --sdk /path/to/Android/Sdk
```

The dependency walk uses the replacement ELF's DT_NEEDED list. The manifest
records its absolute input path and SHA256 as `backend_library_override`;
support libraries are still imported binaries. Keep the corresponding source
revision/build log with the result; a library hash is not source provenance.

The run wrapper only force-stops this dedicated test package, removes its
stale socket files, starts a new Activity, waits for its socket and invokes
the existing WSI runner with the test package's UID. The API probe remains
responsible for verifying the actual connection and protocol. A per-device
host lock prevents overlapping wrapper runs. Afterward the wrapper stops the
test package and records previous/new/remaining PIDs in isolation.json. It
passes --build and --trace through. `--repeat N` (1–100, default 1) starts
N sequential clients without restarting the compositor between them. Each
iteration requires the same compositor PID and `/proc/PID/stat` start time,
records the probe PID and compositor FD listing/count, and references the
full runner result. It stops at the first non-PASS result (including
UNSUPPORTED); the wrapper succeeds only when every requested client passes.
Rebuild the WSI probe for its `WSI_CLIENT` PID evidence. FD snapshots are
observations, not a leak-freedom gate: startup and driver work may affect them.
The run ends with the same package cleanup even on failure. Installation is separate and explicit;
it never replaces the Ardesk package. Closing the test Surface terminates
its process, so the fixture does not reuse a destroyed Surface or native
globals. This also means backgrounding the test Activity may end a run.

For interactive inspection, start the test Activity manually and use
`tests/wsi/run.py --package io.taowen.hybriswsitest --serial SERIAL`.
Repeated manual runs share native state; prefer the wrapper for isolated
iterations. The eight-client binding-table lifetime defect in the original imported backend
is not fixed by isolation; the rebuilt backend revision documented below fixes
surface-owned android_wlegl bindings. A new process prevents its state from accumulating across
wrapper invocations; the repeat option deliberately retains that state within
one invocation, without pretending that long-lived compositor operation
is leak-free. Files/font caches persist; isolation is of process and native
GPU state, not a hermetic filesystem or Android graphics stack.

Initial validation on 2026-09-07 imported backend APK SHA256
b29b7d92d15c223785b66be8934e2983f9113aefeee9e7e55873ad7046fa3553 and generated
APK dcfa145739e5b3dd9582ad0dab92ef8d1621f1bcc8bcb0e090bb325885fe6647.
Both phones installed that separate package via Ardesk's install-apk.sh.
X300 `20260907T095503-db4c5402` and Redmi `20260907T095503-961cc48c`
both pass three sizes, 24 frames and 254,976 screen pixels. Redmi now exercises
actual android_wlegl presentation through this selected backend; its previously
installed Ardesk compositor remains a different, unsupported endpoint.

No standard-loader ICD WSI, presented capture/replay, arbitrary applications,
compositor restart recovery inside a run, long-lived leak freedom or GPU fault
isolation is proved by these fixed-window checks. No unit-test framework is
introduced. Existing host diagnostics accompany each probe result.

Final wrapper validation: X300 isolation `20260907T095811-7b215a7e`, probe
`20260907T095812-bf739cc9`; Redmi isolation `20260907T095811-0c5da649`, probe
`20260907T095812-54cb3f53`. Both are PASS, each with 254,976 matched pixels.
Both started with no existing test PID, used one PID (29659 / 2198), saved
compositor logs (16,777 / 16,885 bytes, not truncated), and ended with no test
PID. Two prior wrapper rounds also passed. Startup now waits for one stable
PID as well as a socket, because a short-lived same-name child was observed
on X300 during startup. The per-device host lock is present but competing
wrapper invocations were not separately exercised. Original Ardesk packages
were not replaced; foreground Surface availability is still shared with Android.

Repeated-client validation on 2026-09-07: the rebuilt probe and `--repeat 10`
produce FAIL on both X300 (`20260907T100827-ad521e8a`, compositor PID 31403,
start time 1532942) and Redmi (`20260907T100827-f9d18538`, PID 5364, start time
4831129). Each uses nine distinct client PIDs, passes clients 1–8 with three
sizes and 254,976 screen pixels per client, then fails client 9's screen gate:
`no expected green-to-red window transition`. Client 9 still reports 24 exact
readbacks and completed frame callbacks. No tenth client runs after failure.
The compositor PID/start time remains unchanged across all nine clients; both
wrappers stop their test process afterward. The earlier run without explicit
client PID/FD evidence reproduced the same eight-pass/ninth-fail boundary.

X300 FD observations are 177 before clients, then
200/202/205/208/211/214/217/220/220. Redmi observations are 151, then
172/166/168/170/172/174/176/178/178. The initial fluctuation is retained rather
than hidden behind a fixed baseline; subsequent growth is consistent with the
source's retained AHB bindings, but these FD counts alone do not identify every
owner or prove the exact runtime slot contents. No backend fix is included.
The same-device wrapper lock was also exercised during the final X300 run:
a second invocation failed with EAGAIN before touching the live test process.

Fresh-process recovery with default `--repeat 1` also passes on both phones:
X300 `20260907T101153-0f7d48b0` / probe `20260907T101154-536a15c5`; Redmi
`20260907T101153-b89a3458` / probe `20260907T101154-17607f09`. Each passes
three sizes, 24 frames and 254,976 screen pixels and leaves no test PID. This
is evidence of recovery after a fresh process, not a remedy for the persistent
backend lifetime defect. No production-library headless suite was rerun for
this probe/wrapper-only change.


Source backend retirement fix: anlabwc
[`25a829e9`](https://github.com/taowen/anlabwc/commit/25a829e98650b786d236584e6c99ae0d9bb60005)
keys android_wlegl AHB bindings by the actual `wlr_surface`, resolves that exact
surface's view, and removes the binding on surface destruction. The removal
releases the retained AHB and immediately updates the Android overlay table,
so its reference need not wait for a subsequent client's draw. Existing
external PID-based presentation APIs are unchanged. Eight simultaneously live
overlay slots remain the limit; concurrent multiwindow/subsurface behavior,
legacy X11/PID callers, abrupt client termination and arbitrary in-flight
resource lifetime are not established by the sequential-client check.

The actual NDK build recompiled embed.c and android_wlegl.c and relinked
libanlabwc.so. `--backend-library` packaged SHA256
6cf3e874f54b40456386ffbf933b166a5d25f2a60d75240d8a4ddbe4ee5995d3 into APK
c24b4fa87e38a21f86863b888954df5f588e9e28e8795b6d3794021565e8c41f.
Only the dedicated test package was updated on both phones. Support libraries
and xkb still came from the same original backend APK; this is not a rebuild
of the entire dependency graph.

Redmi `20260907T101850-d5ce6967` passes all 12 clients in compositor PID 7870
(start time 4893453): 288 frames, three sizes and 3,059,712 screen pixels in
total. Every current client has a `binding retired ... active=0` record; FD
counts after clients are 172 then 164 for the remaining 11 clients. The old
backend failed at client 9 with increasing FDs. The wrapper leaves no process.

Initial X300 `20260907T101850-87d2827b` passes clients 1–10, then fails client
11 (`20260907T102145-a5982141`) under the unchanged strict screen checker:
320×39 pixels differ by one channel value (green 250 instead of 251; red 233
instead of 234). A charging notification is visible in both screenshots;
causality was not independently isolated. The full rectangle and readbacks
remain present, unlike the old ninth-client missing-color failure. This result
remains FAIL; FD counts are 200 after the first client and 199 thereafter,
and each client has a retirement record. It is not counted as a 12-client pass.

Final X300 rerun `20260907T102323-39154d6d` passes all 12 clients with the same
strict checker, compositor PID 7001 (start time 1622605), 288 frames and
3,059,712 matched screen pixels. FD counts are 200 after client 1 and 199
for clients 2–12; every client has its own retirement-to-zero log. No test
process remains afterward. Together with the Redmi result this closes the
observed sequential surface-destruction binding exhaustion, not all compositor
memory/FD lifetime, simultaneous window placement or API compatibility gates.
