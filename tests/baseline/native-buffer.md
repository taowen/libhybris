# Native-buffer foundation for standard ICD WSI

The replacement Wayland frontend uses Android libvulkan's surface/swapchain
implementation. The standard ICD talks directly to the HAL, so it needs its
own swapchain implementation. This batch verifies the underlying buffer import
and synchronization contract; it adds no surface/swapchain advertisement.

`hybris_gralloc_get_hardware_buffer` exposes the Android AHardwareBuffer already
owned by the AHB gralloc backend. The result is borrowed: callers must retain
the gralloc handle throughout use and destroy imports before releasing their
last handle reference. No extra AHB reference is acquired. Unknown handles and
other gralloc backends return NULL. Both tested devices use the AHB backend;
other backend behavior has not been exercised on a device.

The `native-buffer` independent probe enumerates `VK_ANDROID_native_buffer`,
enables it when revision 8 or newer is advertised, and records available usage,
acquire and release entry points. Usage3/4 calls also require their introducing
extension revisions. This is a driver-private protocol, not a public portable
Vulkan application workload; no validation-layer result is claimed.

The probe obtains usage2 flags and converts them through the existing gralloc
conversion functions, preserving the driver's legacy private bits. It adds CPU
read access, allocates a 32×32 RGBA_8888 buffer, and supplies its handle, stride,
usage fields and borrowed AHB to image creation. Native-buffer creation imports
the backing directly: the application does not allocate/bind VkDeviceMemory
for this image. Four rounds acquire, clear to changing colors, copy to a
coherent readback buffer, transition to present layout, and call the Android
release-image operation with a render-completion semaphore.

For every round, all 4096 bytes of Vulkan copy output and all 1024 visible
pixels of the stride-aware gralloc CPU mapping must match the expected color.
Thus the check also establishes that Vulkan wrote the actual allocated native
buffer. Nonnegative release FDs are polled and closed; copies are transferred
to the driver on the next acquire. These incoming copies have already signaled
by that point. This does not prove waiting on an unsignaled acquire FD, GPU
execution overlap, compositor ownership, FD error recovery or leak freedom.
No queue/device wait-idle is used in this probe.

```sh
python3 tests/baseline/run.py --serial SERIAL --icd-hal HAL \
  --vulkan-loader LOADER --case native-native-buffer \
  --case hybris-native-buffer --case icd-native-buffer
```

Use the existing scoped loader quirk for the supported Mali driver.
`native_buffer_fixture.h` is the AOSP native-buffer header at
[frameworks/native 4f463a6b](https://android.googlesource.com/platform/frameworks/native/+/4f463a6b1de9198963dc6aff74154a504ba3f8f6/vulkan/include/vulkan/vk_android_native_buffer.h).
The only adaptation replaces the cutils include with its opaque handle-pointer
typedef; the copyright/license and remaining bytes are retained. Original SHA-256:
`e7197e608685f77aa671a12d9669f48285a1d4c03cdac721ca62338ec339e844`.
The declaration layout supports the observed revisions 8 and 11. The probe
rejects usage2 bits above 32 because the current gralloc allocation API takes
legacy 32-bit usage; that branch was not reached on these devices.

Actual library build completed in 47.707 seconds; Bionic/glibc/linked probe and
Wayland probe builds also succeeded. Final baseline runs retain source/binary
manifests and full logs under `build/results/`:

| Run | Observed result |
| --- | --- |
| `20260907T203715-c882013e` | Adreno 29854870: revision 8, usage2 consumer `0x100`, producer `0x10000200`; allocation usage `0x10000303`, stride 64. Four rounds, zero copied/native pixel mismatches, one nonnegative release FD passed to the next acquire. |
| `20260907T203716-a4f708ed` | Mali 10AFA31610002QH: revision 11, usage2 consumer `0x100`, producer `0x200`; allocation usage `0x303`, stride 32. Four rounds, zero copied/native pixel mismatches, four nonnegative release FDs; three copies passed to subsequent acquires. |

Each run has 2 PASS / 2 UNSUPPORTED including ICD version discovery. Android's
loader and the replacement frontend hide the driver-private extension, so their
probe variants stop before allocation; these are not native import controls.
Earlier query-only runs `20260907T202219-373f6067` and
`20260907T202220-97a912b7` are not counted as import/rendering evidence.

The existing Wayland frontend regression passes on Mali at
`tests/wsi/build/isolated/20260907T203139-c50a5a07` and Adreno at
`tests/wsi/build/isolated/20260907T203356-9e80f519`: surface lifetime exercise,
three window sizes, 24 frames and screenshot/readback checks. Adreno's first
attempt `20260907T203138-4f39a77c` failed compositor PID stability before any
client started; its error record is retained, and the cause is not established.

This is one CPU-readable format/usage combination on one device per vendor.
Native color-attachment draws, non-CPU-readable allocations, protected/shared
images, alias binding, multiple swapchains, acquire timeout/retirement,
standard-loader surface integration and a captured WSI frame remain open.
