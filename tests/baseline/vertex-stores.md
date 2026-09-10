# G1-Ultra vertex stores compatibility

The original Archlinuxcn Blender `17:5.2.1-1` requires
`vertexPipelineStoresAndAtomics`. Its executable is unmodified. The Mali
G1-Ultra driver on X300 reports the feature false and rejects a device request
that enables it. Native diagnostic draws nevertheless execute storage-buffer
writes and integer atomics for visible and fully clipped primitives. With
`rasterizerDiscardEnable = VK_TRUE`, the same driver skips all those writes.

The compatibility layer recognizes vendor `0x13b5`, device `0xe8800010`, driver
`0x0d801000`. It exposes the feature and removes only the emulated feature bit
from the device request passed to that driver. Other driver versions and GPUs
keep their original behavior. `HYBRIS_VULKAN_COMPAT_VERTEX_STORES=0` disables
this path for diagnosis.

For a statically discarded graphics pipeline with pre-raster storage writes,
the layer preserves the shader's execution and replaces the final Position
before vertex/tessellation-evaluation return or geometry emission. An outside
clip-space Position and inert raster/fragment state prevent attachment writes.
Original downstream state can be null; render-pass metadata supplies attachment
count and sample count. Dynamic rendering needs an explicit sample count.
Shaders without storage writes and ordinary rasterized draws use the driver
without this discard compensation. Shader modules remain owned by the caller;
temporary converted modules are destroyed after pipeline creation.

This is bounded compatibility, not a claim of complete Vulkan feature
conformance. Dynamic rasterizer discard with storage writes is rejected at
pipeline creation. Transform feedback, shader objects, graphics pipeline
libraries and dynamic vertex input are unavailable on the matched path.
Unsupported shader encodings or stage extension chains fail explicitly.
Geometry and tessellation rewriting pass SPIR-V validation; the current device
readback fixture exercises the vertex stage only.

## Reproducing the checks

`tests/baseline/build.sh` includes the following explicit cases in its probe
bundle. Pass them to `tests/baseline/run.py --case` with the normal matching
build manifest, runtime, ICD and validation-layer arguments:

- `native-vertex-store-raw`: unsupported native API experiment; expected to
  fail the discarded rounds. It does not claim driver conformance.
- `native-vertex-store-raw-enabled`: the driver rejects feature enablement.
- `icd-vertex-store-validation`: supported application-side API, with Khronos
  synchronization validation placed above the compatibility layer.
- `icd-vertex-store-features2-validation`: the same draws through a Features2
  request, preserving caller-owned chain data and both emulated feature bits.
- `icd-blender-vk-5.2`: original Blender 5.2 feature/extension requirements.

The vertex fixture checks visible, fully clipped and statically discarded
triangles through direct, sparse indexed, indirect and indexed-indirect draws.
It uses two instances, nonzero first vertex/instance, indexed vertex offset,
sentinel indices, guarded SSBO ranges, atomic record writes, and exact pixel
and record readback after a fence. Duplicate vertex invocations are permitted
by Vulkan; every referenced vertex must write, while holes and guards must
remain untouched. The discarded pipeline deliberately omits its fragment,
viewport, multisample and blend state.

The host rewrite test requires a C compiler, Vulkan headers, glslangValidator
and spirv-val:

```sh
VULKAN_INCLUDE=/path/to/Vulkan-Headers/include \
  python3 tests/baseline/test-spirv-discard.py
```

It independently validates generated SPIR-V for Vulkan 1.0 and 1.2, including
vertex early return, missing Position, geometry emission and tessellation
evaluation. It also checks storage-write classification against read-only
input.

## Application evidence and remaining failure

On 2026-09-10, X300 ran original Blender 5.2.1 through Vulkan: startup, the
17-mesh/3808-vertex screwdriver workflow, fullscreen/restore, save/reopen and
640x360 Workbench rendering all succeeded. The renderer identifies
`Mali-G1-Ultra MC12`. Installed Blender SHA-256 equals the signed package's
executable: `6d7ea7012cd80e6ced0ee029c7935c16aa2495bfffc0634f4c9c8694e0bf7c61`.

Eevee remains failed: the same scene writes an almost-black PNG. One-sample
output has RGB at most 1/255; the 64-sample output also has incorrect alpha.
Application-side synchronization validation reports no errors for this run.
Expanding the old upload/readback and rendering-segment policy did not improve
it, so that expansion was discarded. A standard Vulkan capture reproduces the
failure. Replay faults before the requested film-resource dump; no replay
image is claimed as evidence. Workbench success does not establish Eevee or
Cycles support.

`HYBRIS_VULKAN_POLICY_TRACE=1` records the actual application signature,
enabled device extensions and why a bounded application policy was disabled.
It does not enable any workaround or change feature selection.
