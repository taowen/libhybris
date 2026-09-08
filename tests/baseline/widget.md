# Widget UBO probes

The shared [widget fixture](widget_fixture.h) checks a 272-byte block and a synthetic 1232-byte extension. It is not a reproduction of an entire Blender frame. See [multi-descriptor coverage](widget-multi.md) and [capture evidence](widget-capture.md) for additional workloads.

## Dynamic UBO offsets

`ubo-dynamic` retains the 272-byte widget block and uses a dynamic uniform
buffer descriptor. It aligns four slots to minUniformBufferOffsetAlignment,
sets descriptor base=stride and binds dynamic offset=2*stride for good data
at slot 3. Slots 0/1/2 contain the alternate field values; ignoring either
base or dynamic offset therefore changes the expected pixel. A second legal
binding uses dynamic offset=stride and must produce the alternate color.
Native/frontend/ICD run both bindings. `ubo-dynamic-validation` adds VVL and
SyncVal on the ICD path. Base, dynamic offset, range and total size are logged.
This does not enlarge the shader block to 1.2KB, cover multiple dynamic
bindings, descriptor templates or re-record/resubmit, or add dynamic capture
resource reconstruction. Existing static-widget capture cases are retained.

Fresh library/probe builds and runs `20260907T040239-01b5cc15` (29854870)
and `20260907T040239-ec81c1a9` (KB2000) each report 75 PASS, 2 UNSUPPORTED.
Both devices use alignment=64, base=320, dynamic=640 (good) / 320
(alternate), range=272, allocation buffer size=1232. All three routes produce
exact good/alternate pixels, and both ICD dynamic validation variants report
zero errors. Static validation/SyncVal and capture/replay remain passing.
The 1232-byte buffer contains four aligned slots; it is not a 1232-byte shader
block.

## Large UBO layout

`ubo-large` extends the widget shader block to 1232 bytes: the original
272-byte prefix, fourteen column-major mat4 values at 272 (array stride=64,
column stride=16), three vec4 tail values at 1168, signed int at 1216,
32-bit bool storage at 1220 and vec2 end marker at 1224. Host static assertions
check offsets/size; the fragment shader checks every matrix element, tail
vector and end marker, returning magenta on mismatch. Valid blocks encode
bool/int/srgb into the expected good/alternate pixels. The matrices contain
distinct non-symmetric values, so a transpose or incorrect stride fails.
Both ordinary and dynamic descriptors run with both data variants;
`ubo-large-validation` repeats all four draws through ICD VVL/SyncVal.
This is a synthetic layout; it does not reproduce Blender's complete instanced
widget block, descriptor templates, staging copy or command re-record/resubmit.

The large embedded shaders are compiled from `widget.vert` / `widget.frag`
with `glslangValidator -V --target-env vulkan1.0 -DLARGE_UBO=1`, then validated
with `spirv-val --target-env vulkan1.0`. Their SPIR-V decorations confirm the
host offsets above, MatrixStride=16 and matrix ArrayStride=64. The original
shaders compiled without the define remain byte-for-byte identical to their
committed embedded arrays. The build snapshots both source and large embedded
arrays in the probe manifest.

Rebuilt runs `20260907T041010-737e6c4d` (29854870) and
`20260907T041010-39a5c131` (KB2000) each report 79 PASS, 2 UNSUPPORTED.
All four large-UBO variants produce the exact expected good/alternate pixels
on native/frontend/ICD; ICD VVL/SyncVal reports zero errors for each draw.
Dynamic descriptors use base=1280 and offset=2560 / 1280 with range=1232
and total buffer size=5072. Existing static capture/replay also passes.

## Staging and resubmission

`ubo-staged` uploads 272-byte and 1232-byte blocks with vkCmdCopyBuffer from
host-coherent staging buffers into a separate device-local UBO. The target
buffer is never mapped. One descriptor, pipeline, image, readback buffer and
command buffer remain live across six submissions per size. Submissions
0/2/4 record uploads of good/alternate/good data, resetting the command pool
before 2/4 after fence completion; 1/3/5 resubmit the preceding executable command
buffer without recording. Every submission checks the exact expected pixel,
so returning to good data cannot hide an earlier stale result. Explicit
barriers cover previous resource use, transfer-to-uniform reads and readback.
`ubo-staged-validation` repeats both sizes under ICD VVL/SyncVal. This does
not cover descriptor templates, multiple queues, noncoherent staging or
simultaneous pending command buffers. Dynamic descriptors are exercised by
the separate dynamic/large cases, not combined with this staged path.

Rebuilt runs `20260907T041458-a0e027ef` (29854870) and
`20260907T041458-bb55971b` (KB2000) each report 83 PASS, 2 UNSUPPORTED.
Each native/frontend/ICD staged case logs twelve submissions across both
sizes: eight exact good pixels and four exact alternate pixels, with four
successful command pool resets. Both ICD VVL/SyncVal size variants report
zero errors. Existing capture/replay remains passing.

## Descriptor update templates

`ubo-template` uses Vulkan 1.1 core descriptor update templates for both UBO
sizes. Each template has a nonzero input byte offset: the first
VkDescriptorBufferInfo is a valid opposite descriptor, while the second is
the intended descriptor. Ignoring the offset therefore selects the wrong
color. One set/template and the same rendering resources survive six fenced
submissions per size: template updates good/alternate/good accompany command
pool reset/re-recording (reset only after the first recording); each recording
is then submitted twice. Every pixel is checked. `ubo-template-validation`
runs the same path under ICD VVL/SyncVal. This covers one uniform descriptor
entry via the core API; KHR aliases, arrays/stride traversal, multiple bindings,
push descriptor templates and update-after-bind remain unverified here.

Rebuilt runs `20260907T041926-f48b71fa` (29854870) and
`20260907T041926-c42297b8` (KB2000) each report 87 PASS, 2 UNSUPPORTED.
Each native/frontend/ICD template case logs twelve submissions and six
updates with payload offset=24 across both sizes, producing eight exact good
pixels and four exact alternate pixels. Both ICD validation size variants
report zero errors. Staged/dynamic cases and static capture/replay pass.

## Shared fixture layout

Widget layout definitions and good/alternate data construction are separated
into widget_fixture.h, included in the probe source snapshot and manifest.
The helper returns owned 1232-byte storage; the small case uploads only its
272-byte prefix. Both layouts have compile-time size/offset checks. Shader
arrays, GPU call sequence and expected pixels remain unchanged.

Rebuilt runs `20260907T043548-b2be0f97` (29854870) and
`20260907T043548-895d9f1e` (KB2000) each report 88 PASS, 2 UNSUPPORTED.
All widget variants, validation/SyncVal and ordinary/dynamic capture evidence
pass after extraction; fixed attachment divergence remains draw 60 / 61.
This batch reorganizes fixture ownership and does not close additional
compatibility or arbitrary-application diagnosis gates.
