# Historical WSI evidence

This archive preserves earlier device observations and design stages. Its old
commands and endpoints are retired; use [the current runner](README.md).

# Wayland window probe

This optional standalone probe exercises either the replacement Vulkan frontend
or, with `--icd-hal`, the standard-loader ICD against a running Android compositor exposing `wl_compositor`, `xdg_wm_base`
and `android_wlegl`. The default
endpoint is the existing debuggable `io.taowen.ardesk` app's `files/runtime/wayland-0`.
Start that app and its desktop before running; the runner does not install,
restart or update it. A stale socket is not evidence that a compositor is running.

```sh
tools/build-aarch64.sh
tests/wsi/build.sh
python3 tests/wsi/run.py --serial 10AFA31610002QH
```

The build uses the repository's pinned cross-builder and its xdg-shell protocol;
the manifest records generated sources, compiler/package versions, script hash
and binary hash/build-id. Host dependencies are adb, Python 3 and Pillow with
LittleCMS support. The compositor app must permit `run-as`. `--package` and
`--wayland` select another existing endpoint; `--build`, `--probe` and `--out`
select local build/results directories. This is not an APK installer. For existing native-window trace statements,
build with `tools/build-aarch64.sh --debug --incremental --out tests/wsi/build/debug` and run
with `--build tests/wsi/build/debug --trace`. This enables the upstream debug
and trace configure options; runtime tracing alone cannot enable macros omitted
from a release build. `--trace` uses warning-level ordinary logs plus compiled
tracepoints, including producer disconnect counts and retired-buffer releases;
it does not turn on verbose per-hook argument logging. The selected flags and ELF hashes remain in the manifest.

The runner stages a separate directory under the app's files, validates the
hybris/runtime and probe manifests, and retains the exact command, phone
fingerprint, compositor APK hashes, mapped Android library hashes and staged
ELFs. It removes only its own remote directory. The inferior has a 45-second
watchdog; a 65-second host timeout targets its recorded PID only after checking
that PID's current working directory still belongs to this run. Other desktop
processes are left running.

Before rendering, a separate surface lifecycle helper warms one surface and
then starts four threads. Each thread creates four independent wl_surface /
VkSurfaceKHR pairs, waits until all 16 are live, and destroys its pairs in reverse
order. Eight cycles exercise 128 pairs. Thread-start failure releases already
started workers instead of stranding the barrier. The helper roundtrips the
shared display and requires the client process FD count to return to its warmed
baseline. These extra surfaces have no xdg roles or swapchains: this checks
surface metadata lifetime, not multiple displayed windows or GPU buffer release.

The same window renders at 320x240, then 448x288, then 256x192. Each size
submits eight alternating green/red frames using FIFO presentation. Rebuilds
pass the preceding swapchain as oldSwapchain and destroy it after successful
replacement. The probe changes xdg window geometry and checks capabilities
for each size; this is client-initiated resizing, not a compositor drag test. Each acquired swapchain image is cleared, copied to coherent
host memory, checked pixel by pixel, then presented. Acquire semaphores are
reused only after their consuming submit completes; present-wait semaphores are
allocated per swapchain image. Each frame waits for a compositor callback.
QueueWaitIdle is used at rebuild and teardown boundaries. The first/final
frames of each size are deliberately held for screenshots; this is not a frame-rate or nonblocking-presentation test.

The result directory includes:

- `probe.log`: API results, chosen format/color space, image indices and frame callbacks.
- `image-{epoch}-{frame}.rgba` (epochs 0/1/2, frames 0/7): tightly packed RGBA8 readback, normalized from BGRA when necessary.
- `screen-{epoch}-{frame}.png`: actual Android screenshots, with their color profiles preserved.
- `screen-evidence.json`: verifies all six complete readbacks, transforms the expected sRGB primaries
  into each screenshot's embedded ICC profile, and checks that each size occupies its complete expected screen rectangle and
  changes from the expected green to red. It records profile/checker/image hashes and library versions.
  Unknown profiles, scaling, movement within a size pair, or incomplete matches fail.
  Expected dimensions are fixed independently of the probe log.
- `device.json`, `maps-*.txt`, `android-library-hashes.json`, `stage/`: provenance and loaded-library evidence.
- `result.json`: PASS, UNSUPPORTED, FAIL, CRASH or TIMEOUT. PASS requires screen evidence as well as probe exit 0.

On X300 the screenshots carry a Display P3 profile: expected encoded green is
(117,251,76), red (234,51,35). Comparing those bytes directly to sRGB primaries
would misclassify this display. The comparison uses the actual embedded profile,
not these hard-coded values. A frame callback alone is not proof of release or
correct displayed pixels.

This gate does not cover compositor-initiated resize/minimize/out-of-date,
multiple windows, full surface generation tracking, release-fence retirement,
swapchain FD leak accounting,
scaled outputs, arbitrary image contents, standard-loader WSI, validation-layer
chaining on this frontend, or capture/replay of a presented frame. A compositor
that lacks `android_wlegl` now exercises eight surface creation attempts. Each
must return `VK_ERROR_UNKNOWN`, followed by normal instance/Wayland teardown;
the log records `WSI_MISSING_WLEGL rejection=PASS`. The overall window result
remains UNSUPPORTED. This verifies graceful rejection, not rendering support.


## Recorded device runs (2026-09-07)

X300 initial `20260907T075124-f24a7774` advertised android_wlegl version 2,
but Vulkan platform loading aborted because libwayland-egl.so.1 was missing
from the staged runtime. The build now collects the installed ELF dependency
closure instead of a headless-only list. It stages 17 runtime ELFs, including
that library; unused libbsd/libmd are omitted. The interpreter is selected from
the same cross-sysroot search order as libc, and its new hash is in the manifest.

Final X300 `20260907T080229-0e2a8fd6` is PASS: eight presents/readbacks/callbacks,
a complete 76,800-pixel green-to-red display transition at screen bounds
[1219,501,1539,741], and 70 mapped Android library hashes collected successfully.
This validates the fixed window against the recorded APK, not all X300 firmware
or compositor versions. Redmi `20260907T080319-6c3d459b` reports UNSUPPORTED:
its running compositor advertises wl_compositor/xdg_wm_base but no android_wlegl.
No APK was installed or replaced for these runs.

The new runtime was also used for the full headless baseline: X300
`20260907T080304-6c3bd550` has 135 PASS / 10 UNSUPPORTED / 1 CRASH; Redmi
`20260907T080125-f9ffdff6` has 98 PASS / 47 UNSUPPORTED / 1 CRASH.
The remaining crash is native-groups on each phone. Existing validation,
SyncVal and both capture/replay gates pass; X300 ICD cases use the scoped Mali
option. Those captures remain headless and do not capture this Wayland window.


Surface concurrency checkpoint (2026-09-07): the frontend surface map now locks
lookup/insertion/removal, and destruction atomically takes its record before
calling the backend. It does not hold this lock around driver/Wayland operations.
Calls involving the same surface still follow the
[Vulkan external synchronization requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroySurfaceKHR.html).

X300 `20260907T081226-add42893` passes four workers × eight cycles × four surfaces,
with 16 surfaces live at each barrier, client FD count 7 before and 7 after,
and the subsequent eight-frame window/readback/screenshot gate. The old-library
comparison `20260907T081105-efeb0f17` also passes: this is a code-identified map
race fix with exercised regression coverage, not a dynamically reproduced race.
The 67 exported Wayland platform symbol names are unchanged. Redmi
`20260907T081226-1182df69` remains UNSUPPORTED because its compositor does not
advertise android_wlegl. No compositor-side FD count, heap leak measurement,
rendering on concurrent surfaces, resize or release-fence proof is provided by
the extra lifecycle workload; its additional surfaces have no swapchains.

Full headless runs using the rebuilt library: X300 `20260907T081302-cd38e27b`
135 PASS / 10 UNSUPPORTED / 1 CRASH; Redmi `20260907T081302-0f50a7a0`
98 PASS / 47 UNSUPPORTED / 1 CRASH. Native-groups remains the sole crash;
validation, SyncVal and both capture/replay workloads pass.


Discovery failure checkpoint (2026-09-07): old platform run
`20260907T081905-2e8ed7fc` on Redmi aborts with exit 134 when its compositor
lacks android_wlegl. With the final platform, `20260907T082144-82103b66`
returns VK_ERROR_UNKNOWN (-13) on all eight attempts and tears down normally.
The rejection check passes; the overall window remains UNSUPPORTED.
X300 `20260907T082144-8ee218a3` passes the 128-surface concurrency workload
(client FD 7→7), eight-frame readback and complete 76,800-pixel screen gate.
Platform exported symbol names remain identical (67). Allocation errors,
disconnected displays and backend creation errors were not fault-injected.

Full baseline on the final build: X300 `20260907T082214-6907c912` has
135 PASS / 10 UNSUPPORTED / 1 CRASH; Redmi `20260907T082214-648d0d5c` has
98 PASS / 47 UNSUPPORTED / 1 CRASH. Both remaining crashes are native-groups.
Validation, SyncVal and both headless capture/replay gates pass. X300 ICD
continues to use the scoped Mali option.


Resize / reconnect checkpoint (2026-09-07): old frontend
`20260907T082626-c5caa97e` times out (142) during the second CreateSwapchainKHR.
Debug/trace build `20260907T083148-f82d7506` shows native-window disconnect
was a no-op: the new swapchain dequeues three buffers, then waits for the fourth
which the compositor still displays. Vulkan's native window now disconnects
the producer pool. Displayed buffers stay outside the new pool until their
wl_buffer.release; their release does not add a free slot to the new pool.
Other platform implementations retain their default disconnect behavior.
This adds a C++ virtual hook and changes a private buffer helper signature,
so rebuild the complete platform bundle; do not mix old/new platform libraries.
The public Vulkan export set stays at 643 symbols.

Final release-build X300 `20260907T084008-36e56a41` passes 24 frames and screen
rectangles [1219,501,1539,741], [1219,501,1667,789], [1219,501,1475,693].
That is 76,800 + 129,024 + 49,152 = 254,976 matched pixels across three
green-to-red pairs, plus all six full readbacks. The checker rejects a copied
320x240 pair substituted for the larger epoch even with correct larger readback.
Debug build `20260907T084042-7f9dc528` also passes; each reconnect retains one
displayed buffer, followed by its retired-buffer release after new presentation.
This proves the observed Wayland release sequence, not GPU release-fence
retirement, arbitrary in-flight recreation, resize failure recovery or FD
leak freedom for swapchains. Redmi `20260907T083436-db561ef3` passes eight
missing-protocol rejections; its overall window result remains UNSUPPORTED.

The screen gate initially failed after many separate probe processes, including
on the old frontend. The checked compositor source has an eight-slot GPU binding table keyed by
client PID and does not reclaim exited clients. The observed failure and
restart recovery are consistent with exhaustion; the installed APK was not
instrumented to measure its exact live slot count. The API/readback
continued to pass while the displayed content became transparent. After checking
that the test session held only its startup xterm, the X300 test app was restarted
at 08:39; both final runs above use that fresh session. No APK was installed or
modified. This dependency leak remains open: the runner does not restart the
app automatically and must not hide exhausted-compositor failures as PASS.
A process-isolated test compositor and bounded cross-process diagnostics are
still needed for repeated development. The trace runner now avoids the verbose
per-hook debug logs recorded in that initial diagnosis.

Full release-build baseline: X300 `20260907T084136-0606339b` has
135 PASS / 10 UNSUPPORTED / 1 CRASH; Redmi `20260907T084044-c173adc0` has
98 PASS / 47 UNSUPPORTED / 1 CRASH. Both crashes remain native-groups.
Validation, SyncVal and both headless capture/replay gates pass; X300 ICD
still uses the scoped Mali option. No real-window capture or standard ICD
WSI coverage is inferred from those headless checks.

## Automatic failure evidence

The runner starts a PID-filtered compositor log reader before launching the
client. It keeps at most 512 KiB in a rolling memory buffer, records bytes seen
and truncation, then saves `compositor.log` and stops its own adb reader. The
initial one-line logcat tail may predate the run; use timestamps, not mere log
presence, to associate an event. A compositor restart is not followed to a new
PID. Log access failures are recorded in `diagnostics.json`.

After ten seconds without client output, once per run, the runner snapshots the
owned client (PID plus working-directory check) and the compositor. It records
status, mappings, FDs and client thread wait channels, with each text snapshot
limited to 256 KiB and each collection command timed out after five seconds.
`diagnostic-screen.png` is captured before timeout termination. An unsuccessful
client exit also attempts a snapshot, which may report that the client has
already gone. `failure-screen.png` records the screen for failed final results,
including screenshot-checker failures. These diagnostic screenshots do not
replace the six image acceptance checks. This is read-only process evidence,
not a stack unwind, GPU trace, release-fence proof or exact compositor slot count.

`--timeout SECONDS` adjusts the host watchdog (5–300, default 65); the existing
45-second client alarm remains. Collection can add bounded command time beyond
the requested deadline. Failure to collect a diagnostic is recorded separately
and does not turn it into evidence of successful collection. PID-filtered logs
and snapshots may contain other activity within that compositor process.

Final old-library check `20260907T094746-e0dc168e` on X300 used `--timeout 25`
and reproduced the known resize stall: TIMEOUT 124, client main thread in
`do_sys_poll`, 70,618 bytes of client snapshot, 155,310 bytes of compositor
snapshot, and 17,596 bytes of compositor log, without truncation. Both diagnostic
screenshots were saved; the owned client was terminated and its directory
removed. A prior host interruption `20260907T090508-6caffb44` was recorded as
INTERRUPTED after confirming the client had exited; it is not a passing run.

Final current-library X300 `20260907T094825-24bddd09` passes the three-size,
24-frame and 254,976-pixel gate. Redmi `20260907T094825-0f077796` passes eight
missing-protocol rejections and remains window-UNSUPPORTED. Both log reader
processes exited. The unchanged library bundle was rebuilt incrementally in
9.885 seconds; this batch changes host diagnostics only and does not claim a
new full headless baseline. The test desktops had stopped during the interrupted
session and were started again; no APK was installed. Standalone compositor
isolation still requires a separate Android Surface/process lifecycle and is
not implemented by this collector.

A separate [disposable compositor APK](compositor/README.md) now provides the
same fixed-window probe with its own package, process and Android Surface.
Its wrapper restarts only that test package for each run, avoiding pollution
of the Ardesk desktop's native compositor state. Backend binaries are imported
from an explicitly selected APK and hashed; this does not fix the backend's
long-lived binding-table reclamation gap.

For client-exit retirement regression, build the latest probe and run
`python3 tests/wsi/compositor/run.py --serial SERIAL --repeat 10`. The dedicated
compositor remains alive while sequential probe processes exit and reconnect;
all requested runs must pass, with unchanged compositor PID/start time. Each
probe logs its PID, and the wrapper saves compositor FD snapshots between
clients. See [the fixture](compositor/README.md) for failure evidence and scope.


## Native-window ownership split (2026-09-07)

`hybris/vulkan/platforms/wayland/window_owner.{h,cpp}` now owns registry
discovery, its private queue/wrapper, android_wlegl and the native window.
The internal C interface has no Vulkan types or loader callbacks; the frontend
keeps only its VkSurfaceKHR-to-owner mapping and Android Vulkan translation.
The standard ICD compiles the same factory, implements local Wayland
surface create/destroy, and implements `VK_KHR_swapchain` by importing
window buffers with `VK_ANDROID_native_buffer`. Presentation-support is true
only when revision 8 or later of that HAL extension, a graphics queue and usable
color-attachment formats are present. This is an experimental FIFO subset;
see [swapchain review](swapchain-review.md) for supported boundaries and gaps.
Acquire uses a timed native-window dequeue rather than the blocking
ANativeWindow path. It advertises `VK_KHR_surface`/`VK_KHR_wayland_surface`
itself and strips those names before HAL `vkCreateInstance`; `VK_KHR_swapchain`
is advertised on the device and replaced with `VK_ANDROID_native_buffer` for
the HAL. Standard-loader window validation and presented-frame capture are
opt-in on the ICD path; they are not implied by a presentation PASS.

```sh
python3 tests/wsi/run.py --serial SERIAL \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/standard/libvulkan.so.1
```

That ICD path stages `libhybris-vulkan-icd.so.0` and the standard loader,
runs the same `probe-wayland` window/readback/screenshot gate, and records
screen evidence. Missing `android_wlegl` still requires eight
`VK_ERROR_UNKNOWN` rejections.

Windowed Khronos validation and GFXReconstruct capture are separate ICD-only
options. They use the existing standard layer/tools, do not build a private
layer chain, and do not rewrite dispatch headers:

```sh
python3 tests/wsi/compositor/run.py --serial 192.168.1.28:5555 \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/standard/libvulkan.so.1 --swapchain-review \
  --validation-layer tests/baseline/build/validation-build/install/lib/libVkLayer_khronos_validation.so \
  --validation-manifest tests/baseline/build/validation-build/install/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json
python3 tests/wsi/compositor/run.py --serial 192.168.1.28:5555 \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/standard/libvulkan.so.1 \
  --capture-tools tests/baseline/build/gfxreconstruct/install
```

Validation enables `VK_LAYER_KHRONOS_validation` plus SyncVal through
`CreateInstance` and a live debug-utils messenger; any ERROR, including instance
destruction, fails the probe. The error count is safe for concurrent callbacks.
Capture records the live window probe, then dumps the 24 `vkCmdCopyImageToBuffer`
commands through `gfxrecon-replay --swapchain virtual` and compares the six
saved readbacks. That is not a second present, and it does not implement
swapchain image aliasing. The raw `.gfxr`, six compared replay binaries, tool
commands, exit codes and timeout logs are retained in `capture/`. Tool timeouts
clean up their own PID after checking its working directory.

With the pinned tools, `--capture-tools` cannot be combined with either
`--validation-layer` or `--swapchain-review`: the former exposes an injected
extension dependency error; the latter cannot replay the deliberate allocation
callback failure. Both combinations are rejected before device work. Validation
and boundaries can be used together. Layered runs default to a 180-second host
timeout; an explicit `--timeout` remains authoritative. The isolated wrapper
also accepts `--probe` for a separately built probe. See
[the validation/capture review](validation-capture-review.md) for negative
controls, successful separate runs and retained failures.

The frontend destroys the Android Vulkan surface before releasing the owner's
native-window reference, then destroys the window before its protocol objects.
Creation failures clean up the constructed discovery objects. A missing
android_wlegl still maps to VK_ERROR_UNKNOWN in this frontend. The factory
installs both registry listener callbacks and binds only the first matching
global. This batch does not establish allocation-failure safety inside the
existing WaylandNativeWindow constructor or connection-loss recovery.

Actual clean aarch64 build (48.236 seconds) and independent WSI probe build
passed. The built source snapshot matches all four changed build/source files.
The factory object's undefined symbols contain no Vulkan entry points.
The existing dedicated compositor runner passed on both vendor drivers:

| Device | Isolated run / client run | Evidence |
| --- | --- | --- |
| Adreno 29854870 | `20260907T204530-9fd97e0d` / `20260907T204531-878d0df4` | 128 surface pairs, 16 concurrent; warmed client FD count 13 → 13 |
| Mali 10AFA31610002QH | `20260907T204530-1a387e69` / `20260907T204531-1d63c8f7` | 128 surface pairs, 16 concurrent; warmed client FD count 7 → 7 |

Each client passed 24 render/readback/present/frame-callback rounds at three
sizes, all six screenshot comparisons, and normal teardown. Both isolated
compositor identities stayed stable and their owned processes were absent after
cleanup. Raw logs, staged ELF provenance and screenshot evidence remain under
`tests/wsi/build/isolated/`. These historical runs were replacement-frontend regressions. They did not
establish ICD swapchain/present or timeout behavior. Subsequent surface review
is recorded below; current swapchain results are in
[swapchain review](swapchain-review.md). Windowed Khronos validation and
GFXReconstruct virtual-swapchain capture of the three-size copies now have
OnePlus 8T and Mali evidence there; that dump is not a second present.


## ICD surface review (2026-09-07)

The surface-only implementation is preserved in `75953aa`. Its review in
`1b8f9dc` removed the assumption that a graphics queue can present: that revision
had no swapchain engine, so both support queries reported false. The later
swapchain implementation and its corrections are recorded separately. Hard-coded RGBA/BGRA formats,
16384 limits, usages and FIFO success were removed. The required query entry
points remain, but return VK_ERROR_UNKNOWN for unsupported queries. The
[capability query contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceSurfaceCapabilitiesKHR.html)
requires surface support first. The probe therefore checks lifecycle and false
presentation support; it does not interpret fabricated capabilities as coverage.
It also requires a disabled instance-extension entry point to resolve to NULL,
without invoking it in an invalid extension configuration. Instance preparation
no longer silently marks KHR_surface enabled when only the Wayland name appears.

The runner queries the staged adapter before writing `driver.json`, rather than
hard-coding API 1.3. It records that query's command, log and version, and hashes
the staged standard loader. `--icd-mali-loader-quirk` explicitly enables the
existing build-id-scoped MMUD workaround; the isolated compositor wrapper now
forwards the ICD options. Results distinguish `icd-surface-lifecycle` from
`frontend-presentation`. For example:

```sh
python3 tests/wsi/compositor/run.py --serial 10AFA31610002QH \
  --icd-hal /vendor/lib64/hw/vulkan.mali.so --icd-mali-loader-quirk \
  --vulkan-loader /path/to/standard/libvulkan.so.1
```

Actual aarch64 library build (incremental, 16.41 seconds), independent probe
build and Python syntax checks passed. Final source bytes match their build
snapshots. Final device results under `build/isolated/`:

| Path / device | Isolated run / client run | Result |
| --- | --- | --- |
| Standard ICD / Adreno | `20260907T211953-81a53337` / `20260907T211954-2942050b` | Surface lifecycle PASS, presentation unsupported; queried API 1.1.128 |
| Standard ICD / Mali | `20260907T211953-f2055694` / `20260907T211954-c9c34b7a` | Surface lifecycle PASS, presentation unsupported; queried API 1.3.305 |
| Frontend / Adreno | `20260907T211808-60e19b23` / `20260907T211809-79fc403a` | Presentation and screenshot PASS |
| Frontend / Mali | `20260907T211808-11e6c4cd` / `20260907T211809-c9fb93a4` | Presentation and screenshot PASS |

Each run passed 128 surface pairs with 16 concurrently live, with warmed client
FD counts unchanged (Adreno 13, Mali 7). All four compositor identities stayed
stable and owned processes were absent after cleanup. ICD mappings contain the
staged standard loader and adapter, without Android libvulkan. Each frontend
run passed 24 frames at three sizes and six screenshot comparisons. Initial
ICD runs before the manifest-version correction also passed lifecycle checks
(`20260907T211740-24ecd4d9`, `20260907T211741-6391e50c`); the final runs above
supersede them. No standard-ICD rendering, windowed validation/capture, allocator
failure sweep, missing-wlegl device case or non-Wayland build is claimed here.


The current ICD presentation path and `--swapchain-review` boundary probe are
documented in [swapchain-review.md](swapchain-review.md). Its results include
an actual OnePlus 8T / Mali comparison; the earlier Redmi records above remain
historical evidence for their own revisions.


# Historical disposable compositor fixture

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


# Historical standalone X11 fixture

# X11 Vulkan developer probe

This independent tool builds a glibc Vulkan client, a small Android process
supervisor and a private TAWC-DRI Xwayland. It uses the disposable
`io.taowen.hybriswsitest` compositor APK. Each invocation selects one check;
it does not launch the Ardesk desktop or a rootfs. No Mesa changes are needed.
The existing anlabwc android_wlegl backend accepts the imported buffers.

## Build and run

First build the libhybris baseline (`tests/baseline/build.sh`, see its README)
and install the [disposable compositor](../wsi/compositor/README.md).
Supply an Android Xwayland source checkout and an existing NDK dependency
prefix containing `android-cross.ini`, `native.ini`, headers, libraries and
pkg-config files. The builder reads these inputs, builds an isolated source
copy under ignored `tests/x11/build`, and never installs into that prefix.
The tested base is Ardesk's Android-patched Xwayland 24.1.6, commit
`6372702c0c12461787d9fab351912b9663dbcea4`.
Host requirements include Meson/Ninja, Git, tar, patchelf, the baseline pinned
cross-builder container and Android NDK 29.0.14206865.

```sh
python3 tests/x11/build.py \
  --xwayland-source /path/to/ardesk/third_party/xwayland \
  --ndk-prefix /path/to/ardesk/build/ndk-prefix
python3 tests/x11/run.py --serial SERIAL --api xcb --case present \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/glibc/libvulkan.so.1
```

Use `--api xlib` to exercise XOpenDisplay and VkCreateXlibSurfaceKHR. On the
tested Mali device use `vulkan.mali.so` and explicit
`--icd-mali-loader-quirk` (see the ICD README for the firmware-specific scope).
Add both `--validation-layer /path/to/libVkLayer_khronos_validation.so` and
`--validation-manifest /path/to/VkLayer_khronos_validation.json` for VVL and
SyncVal. The runner verifies the enabled layer is actually mapped and requires
zero validation errors through instance destruction.

| Case | Check |
| --- | --- |
| `control` | XCB map/clear/GetImage; environment control, no Vulkan evidence and no loader/HAL arguments required |
| `present` | Standard loader → ICD → vendor HAL, eight alternating GPU clears, full 320×240 image readbacks, two matching physical screenshots, matched protocol release before buffer reuse |
| `missing-protocol` | Use `--server-binary /path/to/unextended/Xwayland`; presentation support must be false and eight surface creations must reject with VK_ERROR_UNKNOWN |
| `acquire-timeout` | Hold all three images, require zero-time NOT_READY and finite 20 ms TIMEOUT, unchanged output index and unsignaled fence |

The supervisor inherits a private Unix listening FD. Xlib uses an additional
private abstract Unix socket; no filesystem `/tmp` or desktop display is
required. It supervises server/client process groups with a 25-second deadline.
The runner uses the same per-device lock as the Wayland fixture, checks
compositor PID plus start time, collects evidence and stops the test package.
The client selects only its tested platform extension and checks that the
other platform's surface entry point is unavailable through instance lookup.

`build/manifest.json` records source/compiler, Xwayland revision, protocol patch,
dependency and ELF hashes. `build/results/<run>/result.json` records staged
inputs, actual loader/HAL mappings, device identity, isolation/cleanup and exit
status. Logs, protocol serials, readbacks and screenshots stay beside it.
Explicit alternate servers are separately hashed. Present screenshots include
two one-second pauses, so elapsed time is developer feedback, not performance.

## Protocol and ownership

The ICD requires local AF_UNIX, a supported TrueColor visual and TAWC-DRI 0.3.
It sends gralloc native handles via PresentBuffer/SCM_RIGHTS and waits for
BufferRelease on its own XCB special-event queue. It does not take application
X event ownership or use PRESENT_SOCKET. Xlib obtains its XCB connection through
XGetXCBConnection. Pending presentations retain native buffer references even
when a swapchain pool is retired; imported Vulkan images also retain theirs.
The shared swapchain layer delegates configure/dequeue/queue/cancel/disconnect
to separate Wayland and X11 owners. Release fences are consumed with a host
wait before submission, matching the existing android_wlegl limitation.
`HYBRIS_X11_TRACE=1` enables at most 64 present/release records per owner.

The vendored `patches/tawc-dri.patch` is copied without modification from
[wmww/tawc](https://github.com/wmww/tawc), commit
`4d74c2db0c9cc927118d493eba32107d7e8971d5`, file
`deps/xwayland-patches/xwayland/02-tawc-step3-ahb-present.patch`.
Its source copyright and MIT license notices are retained. Patch SHA256:
`eba1d87a41943dedb958c27e660ce1f7e1e05cd47d222fde32d366995bc61a46`.
The Xwayland builder disables GLX, glamor, DRM, DRI3 and MIT-SHM; this fixture
exercises native-buffer transport rather than a GL renderer.

## Device evidence, 2026-09-08

All twelve final runs below passed with VVL/SyncVal, stable compositor identity
and empty cleanup errors. Each present run has eight exact GPU image readbacks,
eight protocol presents, seven observed releases and two matching screenshots.

| Device | API | Present | Missing protocol | Acquire timeout |
| --- | --- | --- | --- | --- |
| OnePlus 8T / Adreno | XCB | 20260908T064309-f2149cea | 20260908T064325-a63ec5ab | 20260908T064339-ff13069a |
| OnePlus 8T / Adreno | Xlib | 20260908T064349-a4d95330 | 20260908T064402-92e4bba8 | 20260908T064413-9ceac303 |
| X300 / Mali | XCB | 20260908T064309-430aeca5 | 20260908T064316-d3826e2f | 20260908T064320-a3d63cbc |
| X300 / Mali | Xlib | 20260908T064324-b8ff163b | 20260908T064331-f01225e6 | 20260908T064335-22c9f8de |

Present runs took 7.1 seconds on USB Mali and 12.9–16.2 seconds on wireless
OnePlus; all final cases took 3.8–20.8 seconds including staging and cleanup.
An earlier OnePlus Xlib run `20260908T063020-42610e25` rendered correctly but
failed isolation because startup briefly returned two compositor PIDs. It
remains a FAIL; the runner now waits for one stable PID/start time before use.

The same production build passed both devices' full Wayland swapchain review
with VVL/SyncVal (24 frames, three sizes, surface lifetime, allocator, retirement,
multi-present and timeout checks): isolated runs
`20260908T063331-e8d289ce` / `20260908T063331-29bd303a`.
Headless version, GDPA, lifetime and direct allocator cases also passed:
`20260908T063909-f283b33b` / `20260908T063909-2c7fd235`.

This establishes rootful, single-window, fixed-size XCB/Xlib presentation on
two vendor drivers. Rootless placement, multiple windows, resize/out-of-date,
minimize, disconnect recovery, long-running FD accounting, X11 capture/replay
and CTS remain unverified or unimplemented. The replacement-libvulkan frontend
has no new X11 WSI here. G11 remains open.
