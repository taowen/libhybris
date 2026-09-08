# Experimental BC1–BC3 image fallback

The standard-loader ICD now intercepts application BC image creation, memory
queries/binding, views, transfers and synchronization. This is a partial G07
implementation; it does not provide BC4–BC7 or full Vulkan format conformance.
The separate [decoder probe](bc-decode.md) still tests only the internal kernel.

## Policy and supported domain

The default is off. `HYBRIS_BC_TEXTURES=missing` selects BC1 RGB/RGBA, BC2 and
BC3 UNORM/sRGB formats lacking native optimal sampled-image support. `force`
selects the same eight formats for development comparisons. Environment options
are ignored in secure execution and read once per process.

The initial fallback requires an application requesting Vulkan 1.1 or newer,
a physical device reporting Vulkan 1.1–1.3, and compute support with single-texel
transfer granularity on every transfer-capable queue family. Unsupported
physical devices keep their native format behavior. Vulkan 1.4's additional
state commands are not covered yet.

Selected formats support optimal 2D images, single sampling, mip levels, array
layers and cube-compatible creation, with sampled/transfer usage. Linear,
sparse, mutable, storage, attachment, external-memory and host-copy images
are not advertised. Sparse format and maintenance4 sparse-memory queries return
no requirements for selected emulated formats. The image-format queries use the corresponding native
RGBA8 format's limits and restrict sample counts and usages consistently.
Format properties expose only sampled, linear-filter and transfer support.
`textureCompressionBC` remains false: that feature promises the complete BC
family, including capabilities this fallback does not implement. Individual
formats can be queried separately, as described by
[VkPhysicalDeviceFeatures](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html).

When emulation is active, pipeline statistics and protected memory are disabled;
protected queue flags are removed too. Injected compute must not contaminate
application pipeline-statistics queries. Unsupported descriptor-state and
related extensions are excluded from enumeration and rejected at device
creation: descriptor buffer, push descriptor, shader object, maintenance6/8,
host image copy, QCOM rotated copy, EXT/NV generated commands (including NV
compute generated commands), NV indirect memory copy, and NV dedicated allocation. Feature queries for
the excluded feature structures agree with these decisions. These restrictions
apply only to the opt-in device with selected emulated formats.

## Implementation and resource ownership

Each selected image has a compressed storage/transfer buffer and a native
RGBA8 image. The compressed backing is bound to the application's allocation;
the decoded image has an internal allocation. Dedicated requirements are
queried independently. Image memory requirements, dedicated allocation chains,
BindImageMemory2 device-group indices, and maintenance4's allocation-free memory
queries are translated to the compressed buffer. Application pNext memory is
not modified. Multi-physical-device groups remain unsupported in this mode.

Uploads copy compressed bytes on the GPU, then decode bounded tiles into a
command-buffer scratch buffer and copy those pixels into the RGBA image. sRGB
uses an sRGB RGBA image, so sampling performs the native transfer-function
conversion. Image-to-buffer copies return compressed bytes. Image-to-image
copies transfer those bytes and decode the destination as needed, including
size-compatible native color formats and 2D-array/3D slice copies. The fallback
records commands without submitting queues, mapping upload data or waiting for
GPU completion. It adds memory use and dispatch overhead; performance is not
accepted yet.

The command journal preserves the application's compute pipeline, descriptor
bind history, dynamic offsets and push constants. Compatible pipeline-layout
clones survive application layout destruction until their command recordings
retire. Decoder descriptor sets are immutable for a recording; scratch and
pools are reclaimed on command-buffer/pool reset, free or destruction. Image
barriers and event dependencies also cover the compressed backing, by mip and
array-layer range. Classic and synchronization2/KHR entry points are wrapped.

The modules separate image storage, transfer/decoding, command state, object
registries, resource entry points, image copies, barriers and capability policy.
The existing scaled-vertex implementation retains its own shader/pipeline hooks.

## Probe and reproduction

`bc-images` uses the existing baseline executable and runner. The application
creates two BC images plus a size-compatible native UINT image, uploads data,
copies BC-to-BC and through the native image, samples into an SSBO, and reads
compressed bytes back. A fourth, native RGBA8 image receives the independent
golden palette pixels and supplies a bit-exact sampling reference. Views cover
identity components and an R/B swizzle. The fixture has eight formats, 9×7 and 16×16 base
images, four mip levels and three layers. Native-to-BC copies cover only whole
blocks that fit the destination; partial compressed edge blocks are exercised
by BC-to-BC copies. The aligned shape also exercises native 3D slices.

Each case submits the same recording three times with changed host data first
written after recording. Eligible regions additionally receive a GPU-produced
zero block at a nonzero x offset and array layer. Sampling runs without an
application compute rebind after uploads. The output buffer uses a nonzero
dynamic descriptor offset, allocations use nonzero bindings where permitted,
and selected images force dedicated allocations through a pNext prefix.
Where available, image-format-list chains and maintenance4's pre-creation
memory query are checked against actual image requirements.
The application pipeline layout is destroyed after recording, before submission.
Explicit command-buffer reset, pool reset and free/reallocation alternate with
implicit reset. Classic events are used on both devices; copy2 and
synchronization2 are conditional on queried/enabled extensions.

All 192 readbacks check sampling, compressed bytes and surrounding sentinels.
BC sampling must match the native RGBA8 reference bit for bit. The reference
also matches the fixed UNORM palette exactly; only its sRGB RGB conversion
check allows one 8-bit step against the mathematical transfer function.
Compressed bytes, alpha, UNORM values and untouched sentinels are exact. Validation modes
cover GIPA/GDPA and the standard loader's exported core ELF/link entry points.
KHR aliases use GIPA/GDPA because the standard loader does not export those
aliases as ELF symbols. These modes enable synchronization validation at the
standard loader's application boundary;
they do not inspect ICD-internal commands bypassing that layer.

After building the library and baseline executable:

```sh
python3 tests/baseline/run.py --serial 29854870 \
  --icd-hal /vendor/lib64/hw/vulkan.adreno.so \
  --vulkan-loader tests/baseline/build/icd-spike/libvulkan-loader.so.1 \
  --validation-layer tests/baseline/build/validation-build/install/lib/libVkLayer_khronos_validation.so \
  --validation-manifest tests/baseline/build/validation-build/install/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
  --validation-build-manifest tests/baseline/build/validation-build/install/manifest.json \
  --bc-textures force \
  --case icd-bc-images-validation --case icd-bc-images-gdpa-validation \
  --case icd-bc-images-dlsym-validation --case icd-linked-bc-images-linked-validation
```

For Mali use serial `10AFA31610002QH`, HAL `/vendor/lib64/hw/vulkan.mali.so`
and the explicit `--icd-mali-loader-quirk` flag. Omit `--bc-textures` for the
native-behavior control; use `missing` for the normal opt-in selection policy.

## Evidence and remaining work

The initial application-path run (`20260908T085934-5ae46145`, Redmi;
`20260908T085934-e192caea`, Mali) had correct sampled/compressed readbacks but
failed validation because the probe attempted native-to-BC copies extending
past partial-block mip edges. Those failures are retained. Correcting the
copy ranges and adding aligned images produced two passing validation routes
on each device: `20260908T090636-d9e4612d` and
`20260908T090636-8b7259a8`, each with 192 readbacks per route and zero errors.
Those runs predate the GPU subregion and explicit reset/free additions.

The initial ELF-route expansion (`20260908T101037-190776b8`, Redmi;
`20260908T101037-1d389ca4`, Mali) incorrectly required KHR aliases as exported
symbols. The linked process failed to load, and Mali's dlsym route stopped at
a missing copy2 alias. The probe now uses the required proc lookup for those
extension commands. These failed runs remain recorded.

Final build and device results, 2026-09-08:

| Mode | Redmi 29854870 | Mali 10AFA31610002QH |
| --- | --- | --- |
| `force`, four BC validation routes plus regressions | `20260908T103207-85bb0c79`: 8 PASS, 1 UNSUPPORTED | `20260908T103207-e7a7de4c`: 9 PASS |
| `missing`, GIPA BC validation plus version check | `20260908T103540-d78f7b5c`: 2 PASS | `20260908T103540-c3cae2d6`: 2 PASS |
| Default off, native/frontend/ICD controls plus regressions | `20260908T103831-cae1a003`: 3 PASS, 3 expected UNSUPPORTED | `20260908T103831-a00b3906`: 3 PASS, 3 expected UNSUPPORTED |

The ten enabled BC image executions have 1,920 complete readbacks, zero
mismatches and zero validation errors, including 720 GPU subregion updates.
All use the native RGBA8 reference and exercise the reset/free paths. Both
devices cover image-format-list creation. Mali additionally covers 32 copy2
and 32 synchronization2 cases per execution, and maintenance4 queries matching
actual requirements before every image creation; Redmi does not advertise
those three extensions and does not claim their execution.

The force runs also enable the scaled-vertex fallback. Its validation case,
instance initialization and device lifecycle regressions pass on both devices.
The non-coherent memory-range case passes on Mali; Redmi has no matching
host-visible non-coherent memory type, so its UNSUPPORTED result remains a
coverage gap. Default-off native, frontend and ICD BC image queries all return
unsupported as expected; capability and internal decoder validation cases pass.
No capability is inferred from an expected-unsupported control.

The final clean library build took 55.062 seconds, staged 17 runtime ELFs and
checked 58 installed ELF paths. The three probe binaries were rebuilt using
the existing glibc builder and Android NDK 27.3.13750724. The 67 Vulkan source
inputs in the library manifest and all copied probe source assets match the
worktree; all six final runs retain the same library and probe manifests.
Final SHA256 values:

- ICD: `57600764dece9df9e6709533eb445669efa28490e1bc7377b2695f6197144ac4`
- probe-glibc: `8059aa9d04dee1c68aab4b18f14f1d58e0fd77fdd23987baf2d928482bb772f7`
- probe-glibc-linked: `517a2dcee14863321609edb9c390a7cef98fea0fc5ed2d7a69f5cd6db50b37ed`
- probe-bionic: `61909f89a197124017f8091292bd8dedd9cf27afe27ab181697d18a4b1aef8f8`

Raw logs, mappings, source snapshots and manifests remain under the listed
`build/results/` run directories. The evidence is headless, not a window or
application-rendering acceptance result.

BC4–BC7, signed/float formats, mutable views, general external/sparse/host-copy
resources, excluded state extensions, multi-device groups, all queue-ownership
and aliasing combinations, cube sampling, allocation-failure stress, large
resource limits, performance and CTS coverage remain open. Creation support
or the listed probes must not be read as completion of these requirements or
of G07 as a whole.
