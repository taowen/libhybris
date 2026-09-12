# Original Blender and resized X11 swapchains

2026-09-09, Vivo V2509A / Mali-G1-Ultra MC12. The original Debian
Blender 4.3.2 executable has SHA256
`4b8538be4e595b6150bf4a205c5b74b77e174c3a26d6f96cac7f326ca28899d7`;
`dpkg -V blender` reports no changed package files.

## Failure and diagnosis

The application was started from Arlinux's existing xterm with its normal desktop
environment. GDB's default executable detection saw the explicitly invoked
loader. A known-stack fixture established that mapping-based ELF symbol loading
can recover the application call chain. Applied in the same desktop session,
the Blender main thread was waiting inside the vendor's fence implementation,
reached through the compatibility layer's `wait_fences` with count 1 and an
infinite timeout. Some stripped vendor frames remain unidentified.

A separate GFXReconstruct diagnostic used the same product libraries, with a
verified capture layer before the compatibility layer, page_guard memory
tracking, and write flushing. The 208-record capture prefix contained:

1. Surface extent and swapchain creation at 2521 × 1216.
2. A submitted fence successfully completing its infinite wait.
3. The first acquire returning `VK_ERROR_OUT_OF_DATE_KHR`; no later completed
   fence wait was captured.

The initialized application window subsequently measured 2521 × 1190. The
previous WSI permanently marked the swapchain out of date on a size mismatch,
without acquiring an image or scheduling the requested fence signal. The
application then waited. This explains why library warnings were insufficient
to diagnose the observed hang. Capture IDs and raw handles from different runs
are not interchangeable.

## Change

For a live resized surface whose buffers can still be presented, WSI now returns
`VK_SUBOPTIMAL_KHR`. Acquisition still obtains a real image and signals the
application's synchronization objects through the HAL. Present still queues the
buffer and consumes the application's waits. The X11 producer continues to
recycle released buffers in the existing pool until the application replaces it.

Surface loss, retired chains and genuinely stale native pools remain failures.
A failed acquisition never fabricates a signaled fence or a successful result.
Aggregate present errors take precedence over SUBOPTIMAL. The behavior follows
the [Vulkan WSI acquisition and presentation rules](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html).
No application-name checks or application source/package changes are involved.

## Evidence and limits

The isolated library build differed from the previous deployed source in only
`icd/swapchain.c` and `icd/native_window_x11.cpp`. It was packaged into Arlinux's
APK and the installed ICD hash matched the packaged overlay:
`965f2d5da6756678a8efd9f2650a949be5633f182d9c1583776dd8afdabf3512`.
APK SHA256: `b1e5d792d85d1ca74fc078001b7e06ebe156eed07e71d72e15f53395c4f0c1b8`.
The package database and an existing document retained their pre-upgrade hashes.
The APK also includes the pending general runtime changes from Arlinux; this was
not an isolated APK change limited to the ICD.

Original Blender, launched normally with its supported `--gpu-backend vulkan`
option and a workflow script, completed modeling, save, fullscreen, restore and
reopen. The report identified `VULKAN / Mali-G1-Ultra MC12`. The 17-mesh model had
3808 vertices, 2937 faces and 6 materials. A separate visible Blender process
loaded that saved file, independently checked those counts, and exited zero.
The viewport screenshot showed the modeled screwdriver.

The independent XCB resize probe checked 24 frames at three sizes, screenshot
pixels, two SUBOPTIMAL transitions, successful acquire fence waits, present
semaphore reuse, and 26 protocol presents with matching release-before-reuse
ordering. The Khronos validation run reported zero errors and passed the host
checks. Evidence: Arlinux `build/native-debian/wsi-resize-vvl/20260909T233737-41cc54ef`.
The separate surface-loss run also passed with validation, preserving the
error result, unchanged acquire index, unsignaled fence and present-wait reuse
checks (`wsi-surface-lost-vvl/20260909T233926-427ac8d3`).
The first run's host checker still expected only 24 presents and failed despite
the completed workload; it was updated to require exactly two additional
SUBOPTIMAL presents and rerun. A separate invocation with the wrong validation
manifest failed before executing a probe; neither failed record is relabeled.

This is not the full application/GPU acceptance matrix. OpenGL/Zink on Mali,
long-duration use, remaining devices and the other Debian applications require
separate evidence. Diagnostic runner/session ownership and process identity
gaps remain open; the screenshot is not an exhaustive visual reference test.
