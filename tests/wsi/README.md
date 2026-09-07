# Wayland window probe

This optional standalone probe exercises the replacement Vulkan frontend
against a running Android compositor exposing `wl_compositor`, `xdg_wm_base`
and `android_wlegl`. It does not use the standard-loader ICD path. The default
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
