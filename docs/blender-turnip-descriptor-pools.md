# Redmi Turnip descriptor-pool reset — 2026-09-09

The missing Blender text, images and controls after rendering-segment lowering
have a reproducible Turnip descriptor-pool defect. Fixing the driver restores
the captured UI and permits live small-window editing, saving and OBJ export.
**Application acceptance remains FAIL:** the physical window has transparent
background regions and the ordinary large-window launch still does not appear.
The product launcher and installed driver have not been replaced.

## Localization using the actual capture

The input is `redmi-rendering-layer.gfxr`, captured below the shared layer.
Its 389 rendering segments are ordinary, with nine completed presented frames.
Native Turnip replay produces the same white/missing UI as the live application.
The upload inspector examines zero qualifying noncoherent host-written buffers;
this provides no justification for applying the Mali upload workaround.

Standard resource requests for draws 1394, 1406 and 1423 identify the first
font and widget draws. Vertex data and the 272-byte UBO are nonzero. The font
atlas is R8, 16384×1; texel 26 contains 78. Nevertheless all three 455×26
attachment dumps contain only the background pixel `18 18 18 ff`.
The UBO dump is resource evidence, not proof of shader-visible values.
This staged replay tool predates the no-vertex-attribute index-dump fix, so
the widget index buffer is not covered by this experiment.

`gfxrecon-convert --include-binaries` and `--replace-shaders` permit bounded
controls without changing Blender or the original capture:

| Fragment/vertex control at font draw 1394 | Observed result |
| --- | --- |
| Original shader bytes supplied as replacements | Background only |
| Remove unused vertex output builtins | Background only |
| Constant magenta fragment output | 270 magenta pixels |
| Read vertex color, force alpha one | 270 pixels of `d9 d9 d9 ff` |
| Encode flat glyph dimensions/offset | Expected `12 0f 00 ff` |
| Encode push-constant mask/shift | Expected `ff 0e 00 ff` |
| Fetch atlas texel 26 | Zero instead of 78 |
| Query atlas dimensions and fetched alpha | All zero |

Thus geometry reaches rasterization, while descriptor-based image queries
fail. Ordinary replay with requested `sysmem`, `push_consts_per_stage`,
`flushall` or `noubwc` diagnostics produces the same frame-9 pixels.
No such debug setting is installed as a workaround.

A separately built host replay using llvmpipe (LLVM 21.1.8), offscreen WSI,
memory rebind and omitted pipeline-cache data shows the full startup UI.
The same rebind/cache options on Redmi still fail. Rebuilding the unmodified
Mesa checkout at `bfe5f4ceb76` also reproduces the failure, avoiding attribution
to the unknown source revision of the installed product driver.

## Driver defect and fix

Blender resets descriptor pool 152 at call 225, before allocating its first
font descriptor set at call 294. The pool allows individual descriptor-set
freeing. Turnip initialized its VMA heap with start 32 and **size + 32**;
the utility's second argument is a length, not an end address. Reset also
lost the explicit low-address allocation policy and restored the utility's
high-address default. New set offsets therefore became displaced by 32 bytes,
incompatible with the 64-byte bindless descriptor base. A previously used
pool can read old descriptors; an initially empty reset pool reads zeros.

[Mesa commit 16cd416223d](https://github.com/taowen/mesa/commit/16cd416223d94dd762e283f8215bd4a854efa2e5)
uses one heap-initialization helper for creation and reset, with the actual BO
capacity and the same allocation direction. This fixes the driver rather than
adding descriptor emulation to the shared Vulkan layer. The faulty allocation
code originated in `62f0ef344546`, “tu: Faster descriptor set allocator”.

The existing standalone UBO workload now has `ubo-pool-reset` and
`ubo-pool-empty-reset`, with `-validation` variants. Eight submissions check all
256 pixels against alternating UBO values, two resets, individual free and
reallocation, and reuse of each recording after fence completion. The empty
variant additionally resets the pool before its first allocation. These are
real Vulkan workloads using the existing runner, not new unit tests.

- Product Turnip: fresh pool draws pass, but alternate values after reset/free
  read the previous yellow output. Empty-pool reset yields black in all rounds.
  Both validation variants report zero errors despite failed pixels.
- Patched Turnip: both ordinary and SyncVal variants pass all eight rounds.
- X300 native, hybris frontend and the previously isolated clean ICD: both
  variants pass; ICD SyncVal variants also pass. Runner
  `20260909T171501-b035edd0` has nine passing cases including ICD provisioning.

The ordinary live Blender run with validation explicitly reports SyncVal
enabled and zero errors before the driver fix, while still showing bad pixels.
The separate **offscreen rebind replay** reports four validation errors involving
device-address allocation and a missing instance-extension dependency; that
modified replay path is not a validation pass or application validity proof.

## Application result and provenance

With the fixed driver and unchanged shared rendering layer, the nine-frame
capture restores the startup illustration, text, controls and viewport.
It is not byte-identical to software replay: 17,303 of 480,000 pixels differ,
with RGB maximum channel differences 71/83/84. No exact application pixel
oracle or cross-driver equivalence is claimed.

Live input on Redmi moves the cube to X=3, rotates it about Z by 45 degrees,
scales it to approximately 1.5, and enters mesh editing. Flushing edit data
produces 16 vertices and 12 faces. The saved `.blend`, state JSON and OBJ are
retained. Reopening the saved file returns to the 3D viewport with the edited
mesh visible; physical screenshot `redmi-reopened-check.png` retains the
transparency failure. OBJ world coordinates agree with the state and export axis mapping
within 0.000002 units. This does not establish every requested modal key's
effect, EEVEE output, resize reliability or long-duration interaction.
Physical screenshots expose background transparency, which offscreen RGB
screenshots do not validate. The large-window launch remains unsuccessful;
the previously diagnosed Blender acquire-result handling has not been changed.

Actual ELF SHA-256 identities:

| Runtime | SHA-256 |
| --- | --- |
| Installed product Turnip | `0ae23ffbc4edf1663eacbfa2d74d35e1e1b2f0a131127af9906fb9df382419e7` |
| Rebuilt unmodified bfe5f4ce Turnip | `cb500da84232058b7dc410272bae5f7ebc231269ac932ecd548fcd2baa6eddd9` |
| Fixed Turnip | `b54840a2150d2084edf98d4dd3c964d35ee18da295366d2f13f22d3d5e0a511c` |
| Unchanged shared layer | `1e8ad39ac9a574e7d118fc47fc21f9446e7d3f0f6aeb42cb0355056b1df8de23` |
| Device replay tool | `90d6addb985780bd272384dd98b49e2a04dd312bdfa4ac79b985158cbbc3b642` |

Both Mesa builds use `tools/build/mesa.sh`; the probe builder produced glibc,
linked glibc and bionic binaries. The fixed build used local commit `70be534ea1b`;
its message was amended after validation to final `16cd416223d`. Both have
the identical source tree `311afb501466d20c26509410df3f8a091d50e850`.
The build provenance retains both identities rather than claiming a rebuild.
The replay sources are clean `c2ff0eec`, including the host build.

Ignored artifacts live under Ardesk `build/blender-vulkan/`: `redmi-layer-*`
capture analyses, resource/shader controls and replay outputs;
`redmi-pool-{reset,empty-reset}-before/`, `redmi-pool-reset-after/`, build and
X300 logs; and `redmi-fixed-turnip-*` screenshots, maps, provenance and saved
model/export. Exact launch scripts are in `redmi-descriptor-diagnosis-launchers/`.
Only dedicated `files/run` directories on Redmi were staged. Neither the
separate OnePlus Adreno 830 failure nor the full compatibility checklist is closed.
