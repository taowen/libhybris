# XCB/Xlib client probes

Build the client and Android watchdog with `python3 tests/x11/build.py`.
The only optional input is `--ndk`; the glibc client uses the pinned baseline
cross-builder. `build/manifest.json` records source, compiler, builder and the
two client executable hashes, plus the client's transitive ELF runtime closure.
The existing `tools/stage-runtime.py` collects that closure with the same library
search order as the hybris builder. Staging verifies every hash and rejects
conflicting shared libraries. Turnip keeps the loader/libc pair from its
verified product manifest instead of the client sysroot copies. X11 clients
no longer rely on a removed EGL
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
Swapchain creation requires opaque composite alpha. `fence-acquire` acquires
each frame with a fence, checks its signaled status and a repeated zero-timeout
wait, resets it, and checks that the reset fence is unsignaled. Only then does
it submit the acquired image's layout transitions and rendering. `present`
uses an acquire semaphore instead. Both cases verify eight readbacks, physical
screenshots and native buffer release before reuse.

The [Vulkan WSI contract](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html#_wsi_swapchain)
requires acquisition before using an image, including layout transitions.
The discarded `init-layout` diagnostic submitted transitions for all images
before any acquisition and produced `UNASSIGNED-non-acquired-swapchain-image-used`
validation errors. It is not a valid driver acceptance test. Do not compensate
by eagerly acquiring every HAL image, submitting on a hidden application queue,
or caching fence completion outside the driver.

Device results and artifact hashes are recorded in the
[fence acquisition review](../wsi/fence-acquire-review.md).

The old server builder and vendored TAWC patch have been removed. Historical
results in the [integration review](../wsi/integration-review.md) and
[history archive](../wsi/history.md) describe the previous fixture and do not
prove the externally managed service workflow. Missing-protocol checks now
require an existing display without TAWC-DRI, supplied by its external owner.

## Resize semaphore synchronization (2026-09-08)

The resize probe now waits for the rejected present's queue operations before
re-signaling the same binary semaphore. It also selects the semaphore using
the actually acquired image index, rather than always selecting image zero.
The old probe re-signaled immediately after `VK_ERROR_OUT_OF_DATE_KHR` and
VVL 1.4.362 reported two `VUID-vkQueueSubmit-pSignalSemaphores-00067` errors
on each device. The [pre-change records and controls](../wsi/frontend-removal.md)
remain intact.

The [WSI specification](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html)
requires the semaphore waits to remain enqueued when present returns out of
date; returning from the call is not an observation that those waits completed.
This fixed negative-case probe calls `vkQueueWaitIdle` before the empty
signal/wait submissions. A still-presentable size mismatch queues the held frame as SUBOPTIMAL.
A further acquire that cannot dequeue a new buffer (pool exhausted after
resize) returns OUT_OF_DATE with an unsignaled fence. Surface-lost still
returns `VK_ERROR_SURFACE_LOST_KHR` with an unsignaled fence and unchanged
acquire index. Both paths reuse the same semaphore after `vkQueueWaitIdle`
and preserve old swapchain image handles. The runner requires
`present_wait_idle=1` in both transition records; it does not discard
validation messages.

This probe uses unextended swapchain synchronization. Queue idle here is not
a general replacement for presentation fences or compositor buffer release.
The [Khronos semaphore-reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
describes that distinction. VVL's pinned
[state tracker](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/538f91f14cd39274263eb15e6b4228f355370353/layers/state_tracker/state_tracker.cpp)
clears the old swapchain wait tracking on successful queue idle when neither
swapchain-maintenance1 extension is enabled.

The X11 client was actually rebuilt. Its SHA-256 is
`751d5e9f360a1eb9faec644b4ee32136167ab91c5b9a726f0a55caefafc9018f`.
The verified ICD/runtime and VVL build are the same as the deletion regression;
only the probe and its evidence check changed. No unit tests were added.

| Device / API | Run | Result |
| --- | --- | --- |
| Mali / XCB | `20260908T212159-df46e944` | PASS |
| Adreno vendor HAL / XCB | `20260908T212159-d6bc31a9` | PASS |
| Mali / Xlib | `20260908T212238-f4b7b345` | PASS |
| Adreno vendor HAL / Xlib | `20260908T212238-58838a84` | PASS |

Every run has three sizes, 24 exact GPU readbacks, six matching screenshots,
two complete resize transitions and zero VVL errors. Results are retained in
`/tmp/libhybris-resize-wait-results/`. Python compilation and diff checks pass.
This resolves the reproduced resize-probe validation failure; it is not Turnip
window coverage or completion of all WSI synchronization/teapot gates.

`surface_change.c` shares the image preparation and rejected-present semaphore
check between `resize` and `surface-lost`. The latter destroys the real native
window while keeping its Vulkan surface/swapchain alive, then checks surface
loss and ordinary Vulkan cleanup. It is not a compositor shutdown or concurrent
destruction test. Both cases run through the common WSI backend selector.

The resized-pool WSI now permits SUBOPTIMAL presentation. The resize probe checks
both legal acquisition outcomes: SUBOPTIMAL must return a new image and complete
its fence; OUT_OF_DATE must leave the index untouched and fence unsignaled.
The hybris runner specifically requires SUBOPTIMAL for these live resize cases.
It counts the two extra presents and retains exact pixel, semaphore reuse and
buffer-release checks. Surface loss continues to require its original error
and synchronization behavior. See [original Blender evidence](../../docs/native-blender-resize.md).
