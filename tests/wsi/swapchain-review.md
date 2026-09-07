# ICD swapchain review, 2026-09-07

The user's implementation is preserved in commit `5a2e72d`. The corrective
change retains its real FIFO swapchain path and fixes ownership, synchronization
and capability errors. This is an experimental subset, not full Vulkan WSI
conformance. Windowed Khronos validation and GFXReconstruct capture of the
existing three-size copies now have device evidence.

## Corrections

- `BaseNativeWindowBuffer` is polymorphic. Reinterpreting its output pointer as
  `ANativeWindowBuffer **` skipped the C++ base-address adjustment. The factory
  now receives a derived pointer and converts the actual returned pointer.
- Every imported image retains its native buffer until image destruction.
  Disconnecting an old producer pool cannot release that backing prematurely.
  Already-acquired retired images can still be presented, with a separate native
  reference held until compositor release. Failed replacement attempts also
  retire their old chain. Destruction without a replacement disconnects the
  current pool before a later chain reuses the surface.
- Image creation and destruction use matching allocation callbacks. Device
  cleanup retains resolver state until orphaned swapchain images are released.
  Queue handles are registered at device creation so a present operation can
  resolve its device even with no swapchains.
- Timed dequeue uses prepare-read/read-events/cancel-read on the private Wayland
  queue, with a monotonic deadline and a saturated poll timeout. Polling followed
  by dispatch-pending alone never reads the socket. Zero and finite timeouts
  leave the output index and signal fence unchanged when no image is available.
- Multi-swapchain present consumes application binary semaphores once, using a
  submit that signals one internal semaphore per image. Each Android release
  waits its own semaphore. Merely issuing a preceding empty wait submit is not
  sufficient: an Android driver may ignore the image argument and immediately
  return a signaled fence when its explicit wait list is empty. Single-swapchain
  present passes the original wait list directly. No production queue/device
  wait-idle was added; the existing native queue still waits the release FD on
  the host before committing, because android_wlegl has no per-commit fence
  transport.
- Native dequeue fences are retained for driver acquisition. Queue/cancel FD
  consumption is explicit, including errors, and release wait failures propagate.
  An acquire failure retains the dequeued image as available for retry instead
  of losing it from the pool.
- Formats require a successful color-attachment image query. Optional usages are
  added only when their combination is supported by every advertised format;
  maximum extents come from the same HAL queries. The adapter declares its own
  2–8 images, one layer, identity transform, inherited alpha and FIFO constraints.
  Requests are checked rather than silently clamped or ignored. Native-buffer
  support requires revision 8 or later, and usage bits above the allocation
  backend's 32-bit representation are rejected instead of truncated.
- Single-device group queries and AcquireNextImage2 are provided. Unsupported
  swapchain image-alias creation/binding chains are guarded before reaching a
  HAL that cannot interpret adapter-owned swapchain handles. Full image-alias
  support remains open; the guard is not a conformance claim.
- The private Android header now uses the pinned AOSP declarations and retains
  their license, rather than a second handwritten type subset. The source is
  [frameworks/native 4f463a6b](https://android.googlesource.com/platform/frameworks/native/+/4f463a6b1de9198963dc6aff74154a504ba3f8f6/vulkan/include/vulkan/vk_android_native_buffer.h),
  matching the existing native-buffer probe fixture with platform includes.

Contracts checked against primary sources:
[old-swapchain retirement](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html),
[Wayland read preparation](https://wayland.freedesktop.org/docs/html/apb.html),
and [Android acquire/release protocol](https://source.android.com/docs/core/graphics/implement-vulkan).

## Independent probe

The existing presentation probe now has an optional `--case swapchain-review` runner
mode. It requests Vulkan 1.1 and runs a fixed GPU boundary workload before the
normal three-size window gate. This is not a unit-test framework.

```sh
tools/build-aarch64.sh --incremental
tests/wsi/build.sh
python3 tests/wsi/run.py --serial 192.168.1.28:5555 \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader /path/to/standard/libvulkan.so.1 --case swapchain-review
python3 tests/wsi/run.py --serial 10AFA31610002QH \
  --icd-hal /vendor/lib64/hw/vulkan.mali.so --icd-mali-loader-quirk \
  --vulkan-loader /path/to/standard/libvulkan.so.1 --case swapchain-review
```

The boundary workload acquires all images, checks NOT_READY at zero timeout and
TIMEOUT near 20 ms, then rejects one replacement allocation. It creates a fresh
chain on the now-retired surface, renders an old acquired image and a fresh image,
checks both complete readbacks, and presents both with one application semaphore.
It verifies single-device group commands and the AcquireNextImage2 path. It then
holds every other fresh image, presents another buffer and reacquires the released
image without application Wayland dispatch. Application allocation callbacks must
finish with no unfreed or foreign allocations. The main probe subsequently checks
24 frames at three sizes and all six screenshot/readback pairs.

## Device evidence

At the user's request the Qualcomm comparison uses OnePlus 8T
`192.168.1.28:5555`, Android 13, queried ICD API 1.1.128, replacing the Redmi.
The existing dedicated `io.taowen.hybriswsitest` APK was installed on the OnePlus;
Ardesk was not replaced. Mali remains vivo X300 `10AFA31610002QH`, Android 16,
queried ICD API 1.3.305 with the existing explicit build-id-scoped MMUD quirk.
Each run retains APK/library hashes, command, staged artifacts and device build
fingerprint under `tests/wsi/build/isolated/`.

Original-version build succeeded (incremental, 12.724 seconds), but both actual
ICD window probes failed at first swapchain creation:

| Device | Isolated run / client run | Original result |
| --- | --- | --- |
| OnePlus 8T | `20260907T220458-86a2e778` / `20260907T220500-1f8a12c1` | CRASH 134, native cancel buffer-membership assertion |
| Mali | `20260907T220305-3cbc56c0` / `20260907T220306-4f9d57ea` | CRASH 139, segmentation fault |

Final corrected library build succeeded: clean 48.256 seconds, followed by an
incremental 11.029-second build for the explicit semaphore fan-out correction.
Independent WSI probe build and Python syntax checks passed. Production source
and probe source bytes match their build snapshots. Final runs:

| Path / device | Isolated run / client run | Result |
| --- | --- | --- |
| ICD + boundaries / OnePlus 8T | `20260907T223144-fb8507a9` / `20260907T223146-4fbc38b9` | PASS; finite timeout 20,139,948 ns; socket release reacquired image 0 |
| ICD + boundaries / Mali | `20260907T223144-52c57a33` / `20260907T223145-10dc82fe` | PASS; finite timeout 20,574,848 ns; socket release reacquired image 0 |
| Frontend / OnePlus 8T | `20260907T222210-05206d8b` / `20260907T222211-bc3953b3` | Presentation/screenshot PASS |
| Frontend / Mali | `20260907T222210-3a21e22b` / `20260907T222211-24ec1600` | Presentation/screenshot PASS |

All four runs passed the 128-surface/16-concurrent lifecycle check, with warmed
FD counts unchanged (OnePlus 12, Mali 7), followed by the complete three-size
presentation/screenshot gate. Both final ICD runs also passed every boundary
check above. Standard loader and ICD mappings were observed without Android
libvulkan. All four compositor identities stayed stable and owned processes were
absent after cleanup. Frontend ELF bytes match the final build; the later changes
were confined to the ICD. Surface-lifecycle FD counts are not a swapchain leak
accounting gate.

Headless regression before the final present-only semaphore adjustment passed
on both devices: `icd-version`, `icd-vk-gdpa`, `icd-life`, `icd-vk-alloc` and
`icd-icd-alloc-direct` at baseline results `20260907T221945-7123f9a4` (OnePlus)
and `20260907T221945-c938ec5d` (Mali). Native-buffer and GDPA regression after the
image-chain guards passed at `20260907T222703-cf298183` / `20260907T222703-a18d0b82`.

Mali isolated attempt `20260907T221845-8a575e08` failed its compositor PID
stability check before launching a client; it remains recorded as ERROR, not an
ICD failure or PASS. Its completed retry passed. Intermediate successful window
runs remain in the result archive but do not supersede the final fan-out runs.

## Windowed validation and capture

The existing window probe, not a private layer chain, is the validation and
capture client. Khronos VVL 1.4.309.0 plus SyncVal covers create, render,
client-initiated resize, old-swapchain retirement and destroy, including the
`--case swapchain-review` boundary workload. The first validation runs failed with
12 probe-side acquire-wait / SyncVal errors; after waiting acquire at
`VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` and giving the review presents dedicated
fences, both devices reported `WSI_VALIDATION errors=0`.

GFXReconstruct 1.0.5 captures that same three-size window, then
`gfxrecon-replay --swapchain virtual` dumps the 24 `vkCmdCopyImageToBuffer`
commands. The six saved live readbacks match the replayed copies byte-for-byte.
The first capture attempts recorded the window and replayed 24 buffers, then
failed because dump-resources JSON pads earlier `transferCommands` slots with
null; flattening those arrays counted 300 entries. The verifier now indexes
non-null commands by `cmdIndex`. Replay did not use `VkImageSwapchainCreateInfoKHR`;
swapchain image alias remains unsupported.

| Path / device | Isolated run / client run | Result |
| --- | --- | --- |
| ICD + VVL + boundaries / OnePlus 8T | `20260907T225316-dcafbe9f` / `20260907T225317-d03a43fc` | PASS; `WSI_VALIDATION errors=0` |
| ICD + VVL + boundaries / Mali | `20260907T225316-bf0c6ea9` / `20260907T225316-633801ff` | PASS; `WSI_VALIDATION errors=0` |
| ICD + capture / OnePlus 8T | `20260907T230116-0e424415` / `20260907T230117-6e85604f` | PASS; 24 copies / 24 presents; six readbacks match |
| ICD + capture / Mali | `20260907T230116-2c517ce0` / `20260907T230117-bdcd72bc` | PASS; 24 copies / 24 presents; six readbacks match |

Capture is a virtual-swapchain dump of the probe copies, not a second present
or an application replay. Both compositor identities stayed stable.

The subsequent [validation/capture review](validation-capture-review.md) fixes
a reproduced teardown ERROR being reported as PASS, preserves raw capture inputs
and replay binaries, and records the incompatible option combinations. Its
corrected-probe runs supersede the validation/capture verdict handling above.

## Remaining gaps

Complete image-alias support, protected/multi-device presentation, presentation
extensions, arbitrary application conformance, compositor restart/minimize
recovery, full allocation failure sweep or comprehensive FD/resource leak
accounting is not claimed. The two swapchains in the boundary workload share
one surface; this is not a two-window screen comparison. The native-window
constructor still has pre-existing allocation assertions. Native dequeue
currently supplies -1 fences; unsignaled import-FD waiting and GPU execution
overlap are not established by these tests.
