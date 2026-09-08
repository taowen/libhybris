# BC1 RGB border correction and remaining Redmi failure

The BC1 RGB image fallback now chooses native RGB8 UNORM/sRGB storage when
the physical device reports optimal sampled and transfer support. This fixes
the tested Mali border/default-alpha behavior without changing color bytes.
Redmi lacks that native format support and still fails the added border probe.
This batch is a partial correction, not complete BC1 RGB or G07 acceptance.

## Failure and implementation

The previous RGBA8 backing supplied correct in-bounds alpha but acquired the
sampler's transparent-black alpha outside the image. Linear filtering across
the edge therefore returned alpha 0.5 or 0.25 where BC1 RGB requires its absent
alpha component to remain one. The distinction follows Vulkan's
[border replacement and component swizzle rules](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-border-replacement).

The unchanged old library with the new probe failed on both devices:
Redmi `20260908T112251-5cd9aaac`, Mali `20260908T112251-d2e872b5`.
Each image execution completed 288 readbacks, with 306 mismatched words in
12 BC1 RGB UNORM/sRGB transparent-border rows, and zero validation errors.
For example, `80004040` was observed where `ff004040` was expected.
These are retained FAIL results; earlier passes did not cover this behavior.

The same runs queried native formats. Mali supports optimal RGB8 UNORM/sRGB,
RGB16 UNORM/float16 and RGB32 float images. Redmi reports no optimal image
support for those formats; RGB8 UNORM has linear-tiling features, which do not
establish the required optimal sRGB/mip/array domain. No lower-precision format
is substituted to make the comparison pass.

Selection is based on native format properties, separately for UNORM and
sRGB, and retained in each device/image. Format and image-limit queries, image
creation, view creation and upload encoding use the same choice. RGB8 output
packs four texels into three storage words. One invocation owns those words,
including across odd rows/layers, and zero-fills a partial last word. The
existing RGB565/interpolation bytes and native sRGB conversion are unchanged.
The output uses three bytes per texel; the existing bounded scratch allocation
still budgets four. There is no CPU texture decoding or extra queue submit.

On Redmi, the previous RGBA8 alternative remains, including its failure. Merely
forcing the native view's alpha swizzle would put otherwise-defined opaque-black
sampling into the backend's undefined nonidentity-swizzle domain. That is not
used as a portable fix. A correct path for devices lacking native RGB8 remains
implementation work; this is not an external-device blocker.

## Probe and final evidence

The existing image probe now also checks BC1 RGB linear filtering with
clamp-to-edge and all three fixed floating border colors. It compares all
sampled components against an independently uploaded native RGBA8 image and
requires alpha one. The reference view forces alpha one only where the swizzle
has defined behavior; opaque-black cases use identity views. The latter also
use identity views for the BC image. The CPU sampling/sentinel oracle now lives
in `bc_image_verify.h`, separate from Vulkan recording. Existing BC4/BC5
precision thresholds, raw-byte checks and failure classifications are retained.

The internal decoder probe adds two RGB8 output encodings. Its 224 readbacks
cover the original twelve encodings plus both RGB8 variants, odd packing,
resubmission, GPU-written input and dispatch splitting. One additional negative
case rejects RGB8 output for BC1 RGBA. Kernel success does not prove that a
native RGB8 image is available.

Final runs from one library/probe build:

| Mode | Redmi 29854870 | Mali 10AFA31610002QH |
| --- | --- | --- |
| `force`, four image validation routes and regressions | `20260908T115229-9141564d`: 4 PASS, 4 FAIL, 1 UNSUPPORTED | `20260908T115229-47532dcb`: 9 PASS |
| `missing`, image validation and version | `20260908T115251-2b51ec39`: 1 PASS, 1 FAIL | `20260908T115251-de14c965`: 2 PASS |
| Default off, capabilities, kernel routes and image controls | `20260908T115258-623972c7`: 7 PASS, 3 UNSUPPORTED | `20260908T115257-faf95536`: 7 PASS, 3 UNSUPPORTED |

Mali's five enabled image executions pass all 1,440 full-buffer readbacks,
including 240 BC1 RGB filtered readbacks, with zero mismatches and validation
errors. Each covers 48 copy2 and synchronization2 cases, maintenance4 queries,
format lists, descriptor-pool rollover and the existing retirement paths.

Each corresponding Redmi image execution remains FAIL: 12 of 288 readbacks
have the same 306 mismatched words in BC1 RGB transparent-border rows; the
other 276 readbacks have zero mismatches. Validation reports zero errors.
These failures are not changed to UNSUPPORTED or omitted from the runner's
exit status. Device lifecycle, initialization and scaled-vertex validation
pass on both devices. Non-coherent memory-range validation passes on Mali;
Redmi still has no matching host-visible memory type.

The six default-off native/frontend/ICD kernel executions total 1,344 passing
readbacks, including 192 RGB8-output readbacks. The ICD kernel route has zero
VVL/SyncVal errors. Default-off image queries still report all twelve formats
unsupported on both devices, while native/frontend/ICD capability probes pass.

The final clean library build took 60.537 seconds, staged 17 runtime ELFs and
checked 58 installed ELF paths. The shader passed Vulkan 1.0 SPIR-V validation;
probes used the existing glibc builder and NDK 27.3.13750724. All 72 Vulkan
source/header/include/build-rule inputs and all 85 probe source assets match
the worktree. All six runs retain identical library/probe manifests.

Final SHA256 values:

- ICD: `865db0908df892339ca8c616ef44fe19d32295668a2df107819dee1becf332ac`
- probe-glibc: `13e6bfe71c11f8649f9ebc4cc4427b9a95a39f5fa1e8e43183ea5cad2feaf381`
- probe-glibc-linked: `e0fbfb2527816f237f1c470962e7dbe465f60206beaa305df91e5d37b8f0462e`
- probe-bionic: `25bdd469f962e12c2d44c3652897a1c55795184b7354cac9a8045876620dc843`
- decoder include: `fb2d4ac27e4f1e7632fca949fac434ff31142e9d1da1d072dcf61c04c684ef1d`

Use the [existing image-run commands](bc-images.md#probe-and-reproduction),
including the explicit scoped Mali loader option on that device. The expanded
case is expected to return FAIL on this Redmi build. Custom border colors,
all sampler/view combinations, cube/gather behavior, maximum limits and
performance remain outside this fixed-border fixture. BC6/BC7 and the other
G07 requirements remain open.
