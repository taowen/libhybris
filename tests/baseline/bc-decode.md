# Internal BC1–BC3 GPU decoder

`hybris/vulkan/compat/bc_decode.c` provides a Vulkan 1.0 compute kernel for the
8 BC1 RGB/RGBA, BC2 and BC3 UNORM/sRGB formats. The output is tightly packed
RGBA8. sRGB output retains encoded RGB bytes; a future sRGB image performs the
sampling conversion. BC4–BC7 are rejected. The block interpretation follows
the [Vulkan compressed-format mapping](https://docs.vulkan.org/spec/latest/appendices/compressedtex.html)
and the S3TC chapter of the Khronos Data Format Specification linked there.
The kernel expands RGB565 endpoints by bit replication and uses integer
interpolation with truncation. The fixtures establish the listed byte results,
not comprehensive format precision conformance.

This is an internal kernel. **BC image interception is not connected to the
ICD, and no format properties or `textureCompressionBC` declarations change.**
The probe explicitly creates and records the decoder from the same production
C source, snapshotted into its build bundle. Its Vulkan calls run through the
native, frontend and standard ICD routes. `image_interception=0` is part of the
summary so this evidence cannot be read as application BC support.

The decoder consumes two storage-buffer descriptors. Source offsets, padded
block rows/layers, destination offsets, dimensions and range arithmetic are
checked before recording. Output size must fit one device storage-buffer
range. Large dispatches are split at the device's X workgroup-count limit.
Unsupported/internal invalid inputs return an error before any command is
recorded. The caller supplies descriptors and synchronization, retains all
resources until commands retire, and owns the compute pipeline, descriptor
and push-constant state. An ICD interceptor still needs state preservation
before it can use this API. No host mapping, queue submit or wait occurs in the
kernel helper.

## Existing baseline case

`bc-decode` and `bc-decode-validation` use the existing executable and runner;
there is no separate test harness. Each execution checks 128 readbacks:
8 formats × 4 region shapes × 4 submissions. Shapes are 4×4×1, 9×7×3 with
16-texel rows/12-texel layer height, 1×1×2, and 129×5×2 with 144/12 strides.
The last shape reduces the caller's dispatch limit to one workgroup to exercise
splitting without allocating a device-limit-sized buffer.

Fixed palettes cover both BC1 endpoint orders, equal endpoints, RGB versus
RGBA black/transparent selection, BC2's forced four-color mode and all explicit
alpha values, both BC3 interpolation branches, equal alpha endpoints, all
alpha indices including the cross-word index, and non-integral interpolation.
Every output word is compared, including prefix/suffix sentinels.

The upload buffer has transfer usage only. A recorded transfer copies it into
storage-buffer scratch before compute. Host data is first populated **after
recording**, then changed between three submissions of the same command
buffer. The fourth submission records a GPU fill of the upload buffer and
checks the resulting zero block. Buffers have nonzero memory binding offsets;
mapped allocations are flushed/invalidated in full. Ten live-recording
rejection controls cover BC4/BC7, short ranges, offset/dimension overflow,
invalid strides and unavailable dispatch/range limits. SyncVal is enabled in
the validation case.

From the repository root, after the normal library and baseline builds:

```sh
python3 tests/baseline/run.py --serial 29854870 \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader tests/baseline/build/icd-spike/libvulkan-loader.so.1 \
  --validation-layer tests/baseline/build/validation-build/install/lib/libVkLayer_khronos_validation.so \
  --validation-manifest tests/baseline/build/validation-build/install/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
  --validation-build-manifest tests/baseline/build/validation-build/install/manifest.json \
  --case native-bc-decode --case hybris-bc-decode \
  --case icd-bc-decode --case icd-bc-decode-validation --case icd-caps
```

For Mali use serial `10AFA31610002QH`, HAL `/vendor/lib64/hw/vulkan.mali.so`
and the explicit `--icd-mali-loader-quirk` flag. The loader/layer paths above
are existing local build products, with their hashes retained by the runner.

## Recorded evidence, 2026-09-08

A clean AArch64 library build and glibc/linked/bionic probe builds succeeded.
The final runs retain manifests, snapshots, binaries, device details and logs
under `build/results/`:

| Run | Device | Result |
| --- | --- | --- |
| `20260908T074743-e77c29e8` | Redmi 29854870, Adreno | 6 PASS: version, capabilities, four decoder routes |
| `20260908T074743-1fca013e` | 10AFA31610002QH, Mali | 6 PASS, same cases with explicit MMUD opt-in |

Each decoder route has 128 complete readbacks, zero mismatches and all ten
expected rejection results. Both validation cases have zero errors. Both
capability cases report `textureCompressionBC=0`. The final probe-glibc hash is
`0d76b73941955925d590bb4d6ce24ed72688d6c94f61fddf70012fb8f24d2d37`;
the library ICD hash is
`6594b17166dc0478554ace117f6c19c231962ae3b1b066f588e375a8e9b3dec2`.

`compat/shaders/generate-bc.py` invokes glslang for Vulkan 1.0 and spirv-val;
normal builds consume the checked-in SPIR-V and need neither tool. Regeneration
with glslang 16.2.0 and SPIRV-Tools 2026.1 reproduced the asset byte-for-byte:
`bc_decode.inc` SHA-256
`872dba9582d5bae48723cc3d3a8ba124ab634328a95c2b7e2e4027b4b9fd5b13`.
This records the actual host generator versions, not a pinned shader-generator
build. The library/probe manifests separately fingerprint their source inputs.

Image creation/binding/views, actual mip/array image subresources, image copy
and compressed readback, sRGB sampling/filtering, application compute-state
restoration, cross-family execution, BC4–BC7, allocation failure injection,
maximum-range stress, performance and applications remain unverified or
unimplemented. G07 remains open.
