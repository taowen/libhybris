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
python3 tests/wsi/compositor/run.py --serial SERIAL
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

The run wrapper only force-stops this dedicated test package, removes its
stale socket files, starts a new Activity, waits for its socket and invokes
the existing WSI runner with the test package's UID. The API probe remains
responsible for verifying the actual connection and protocol. A per-device
host lock prevents overlapping wrapper runs. Afterward the wrapper stops the
test package and records previous/new/remaining PIDs in isolation.json. It
passes --build and --trace through. Installation is separate and explicit;
it never replaces the Ardesk package. Closing the test Surface terminates
its process, so the fixture does not reuse a destroyed Surface or native
globals. This also means backgrounding the test Activity may end a run.

For interactive inspection, start the test Activity manually and use
`tests/wsi/run.py --package io.taowen.hybriswsitest --serial SERIAL`.
Repeated manual runs share native state; prefer the wrapper for isolated
iterations. The eight-client binding-table limit in the imported backend is
not fixed here. A new process prevents its state from accumulating across
wrapper invocations, without pretending that long-lived compositor operation
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
