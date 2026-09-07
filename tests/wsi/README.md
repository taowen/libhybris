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
select local build/results directories. This is not an APK installer.

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

The fixed 320x240 window submits eight alternating green/red frames using
FIFO presentation. Each acquired swapchain image is cleared, copied to coherent
host memory, checked pixel by pixel, then presented. Acquire semaphores are
reused only after their consuming submit completes; present-wait semaphores are
allocated per swapchain image. Each frame waits for a compositor callback.
QueueWaitIdle is used only at teardown. The first/final frames are deliberately
held for screenshots; this is not a frame-rate or nonblocking-presentation test.

The result directory includes:

- `probe.log`: API results, chosen format/color space, image indices and frame callbacks.
- `image-0.rgba`, `image-7.rgba`: tightly packed RGBA8 readback, normalized from BGRA when necessary.
- `screen-0.png`, `screen-7.png`: actual Android screenshots, with their color profiles preserved.
- `screen-evidence.json`: verifies both complete readbacks, transforms the expected sRGB primaries
  into each screenshot's embedded ICC profile, and checks the same complete 320x240 screen region
  changes from the expected green to red. It records profile/checker/image hashes and library versions.
  Unknown screenshot profiles, scaling/movement or incomplete matches fail this fixed gate.
- `device.json`, `maps-*.txt`, `android-library-hashes.json`, `stage/`: provenance and loaded-library evidence.
- `result.json`: PASS, UNSUPPORTED, FAIL, CRASH or TIMEOUT. PASS requires screen evidence as well as probe exit 0.

On X300 the screenshots carry a Display P3 profile: expected encoded green is
(117,251,76), red (234,51,35). Comparing those bytes directly to sRGB primaries
would misclassify this display. The comparison uses the actual embedded profile,
not these hard-coded values. A frame callback alone is not proof of release or
correct displayed pixels.

This first gate does not cover resize/minimize/out-of-date, multiple windows,
multiple surface generations, release-fence retirement, FD leak accounting,
scaled outputs, arbitrary image contents, standard-loader WSI, validation-layer
chaining on this frontend, or capture/replay of a presented frame. A compositor
that lacks `android_wlegl` reports UNSUPPORTED, not a passing Vulkan/WSI result.


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
