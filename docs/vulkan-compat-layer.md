# Shared rendering compatibility above native Turnip

The explicit `VK_LAYER_HYBRIS_compat` layer reuses the HAL ICD's rendering
segment transformation through the standard Vulkan loader. In the Redmi
Blender 4.3.2 small-window reproducer it passes the original
`tu_insert_dynamic_cmdbufs` crash and reaches successful presentation.
With the original driver, the application still displays missing content/white
regions. The [descriptor-pool follow-up](blender-turnip-descriptor-pools.md)
fixes that driver defect in an isolated build and restores UI content; physical
window transparency was subsequently fixed in the
[shared window transport](blender-opaque-wsi.md), and the layer/driver are now
deployed through the Redmi product launcher. **Application acceptance remains
FAIL because default-size startup and the full application gate remain open.**

## Ownership and activation

`hybris/vulkan/compat/rendering_segments.c` now contains only the common
transformation. `icd/rendering.c` retains the existing HAL adapter. The layer
has separate instance/device dispatch (`layer/entry.c`) and command/pool
lifecycle (`layer/commands.c`); it does not load Android libraries, implement
WSI, wrap handles, or alter advertised device capabilities. The built layer's
only DT_NEEDED entry is `libc.so.6`.

The layer matches the existing Blender application/engine signature and API
1.2, then applies a rendering-only profile to devices reporting
`VK_DRIVER_ID_MESA_TURNIP`. That public signature is not a reliable Blender
release-version check. Unknown device extensions and multi-physical-device
groups disable the profile. Its extension set additionally permits the
observed `VK_EXT_shader_stencil_export`; this does not enable the HAL
upload/readback profiles for that extension set. Other applications and
drivers pass through. Loading the layer over the hybris ICD therefore does
not duplicate its existing rendering transformation.

The common transformation retains intermediate attachment contents with
LOAD/STORE, suppresses resolves in suspended segments, and inserts a command
dependency before each ordinary segment. It does not use queue-idle waits.
Allocation failures remain sticky until command-buffer/pool reset or a new
recording, and are reported by EndCommandBuffer. Command metadata uses its
pool's allocation callbacks. Instance/device records use their creation
callbacks. Device generations distinguish reused owners; driver calls and
allocation callbacks execute outside metadata locks.

The adapter follows the standard
[loader/layer interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md)
version 2. Instance/physical-device and device/command-buffer dispatch keys
select their corresponding next resolver. Intercepts preserve next-layer
availability; unhandled commands and unknown physical-device extension
queries continue through the chain. The application-visible API is still
provided by the standard loader, not by direct exports from this layer.

## Build and select the layer

The existing AArch64 builder now produces `libVkLayer_hybris_compat.so` and
the colocated `VkLayer_hybris_compat.json` under `install/usr/lib/hybris`:

```sh
tools/build-aarch64.sh --incremental
VK_LAYER_PATH=/path/to/install/usr/lib/hybris \
VK_INSTANCE_LAYERS=VK_LAYER_HYBRIS_compat \
VK_DRIVER_FILES=/path/to/freedreno_icd.json application
```

Keep the product's standard loader search path. `VK_LAYER_PATH` selects the
manifest; adding the hybris library directory to `LD_LIBRARY_PATH` could
instead select its Android Vulkan frontend with the same loader SONAME.
The layer is explicit and is not globally activated by installation.

For a capture of the transformed calls, the tested order is
`VK_LAYER_HYBRIS_compat:VK_LAYER_LUNARG_gfxreconstruct`, with both manifest
directories in `VK_LAYER_PATH`. The capture layer is below compatibility.
Validation below compatibility checks the generated stream; validation above
it sees the application's original stream. Preserve the chosen order as part
of the evidence.

## Actual builds and device checks — 2026-09-09

The main checkout build passed in 45.922 seconds. A separate checkout at
`211cccc` with only this batch's Vulkan/library-build changes passed a clean
build in 43.028 seconds, without the unrelated WSI/fence/shader edits in the
main worktree. Both produced the same layer SHA-256:
`1e8ad39ac9a574e7d118fc47fc21f9446e7d3f0f6aeb42cb0355056b1df8de23`.
The final clean-checkout incremental build, including the ICD allocation
output correction below, passed in 7.927 seconds. The layer bytes did not
change with that ICD-only correction.

The existing baseline builder compiled the added standalone pixel probe and
the extended allocator/ownership probes for glibc, linked glibc and bionic.
These are real Vulkan workloads, not unit tests or a new runner framework.
The pixel probe uses a legal three-part suspend/resume chain, different
rectangular clears, four rounds with changing colors, pool reset and an
explicitly synchronized/invalidate-backed 2048-pixel readback each round.
Retaining CLEAR in the original resume info checks that lowering substitutes
LOAD rather than erasing prior segments.

The executable modes are `render-segments`, `render-segments-gdpa`,
`render-segments-control` and their `-validation` variants, plus
`command-alloc-blender` and `render-owners-blender`. The control changes the
application name to disable the policy. For the HAL ICD, select these as
`icd-<mode>` in the existing baseline runner, including `icd-version` to
provision its manifest. `native-render-segments-control` is the bionic
comparison. For native Turnip, run the same glibc executable with the product
standard loader/ICD and the layer environment above; no Android HAL bridge
is needed. Preserve normal Vulkan runtime dependencies and the validation
manifest path when requesting validation modes.

| Device / path | Result |
| --- | --- |
| Redmi product Turnip, no compatibility layer | Four exact pixel rounds PASS |
| Redmi with layer, unmatched application signature | Four exact rounds PASS; profile inactive |
| Redmi active layer, KHR GIPA and GDPA | Both four-round pixel runs PASS |
| Redmi active layer, KHR GIPA and GDPA with SyncVal | Both PASS, zero validation errors |
| Redmi inactive layer control with SyncVal | PASS, zero validation errors |
| Redmi active layer, allocator rejection/recovery | PASS: partial metadata allocation freed, all output command handles null, final live allocation count zero |
| Redmi active layer, two device owners | Six child-lifecycle cycles PASS; one device destroyed/recreated while the other remains live |
| Redmi inactive layer, dispatch and threaded lifecycle | Both existing probes PASS |
| Redmi inactive layer, core 1.3 rendering with validation | PASS |
| X300 native legal chain and clean ICD lowering | Exact pixel rounds PASS; ICD GIPA/GDPA pass; profile/control validation runs have zero errors |
| X300 clean ICD existing seven-round readback validation | PASS |

The product standard loader has no ELF export for `vkCmdBeginRenderingKHR`:
the new ELF route is UNSUPPORTED. The old `render-owners` probe also stops
when it reaches its ELF KHR route; only its first cycle completes. The new
profile variant uses GIPA/GDPA and legacy submission to exercise the active
layer's ownership metadata without pretending that the ELF route passed.

The first X300 runner attempt (`20260909T162338-db6f9e56`) scheduled the two
non-validation new cases before ICD manifest provisioning; those fail with
exit 2 and remain recorded as failures. The runner order was corrected.
`20260909T162654-d139baf2` then passes version, GIPA/GDPA pixels and active
owner recreation. The first run's later validation and existing readback
cases had already passed after provisioning.

The new allocator profile also exposed an existing HAL ICD failure: adapter
metadata rejection left output handles unchanged (`all-null=0`). The ICD
now initializes the whole output array before staging metadata, as required
by [vkAllocateCommandBuffers](https://docs.vulkan.org/refpages/latest/refpages/source/vkAllocateCommandBuffers.html).
`20260909T162912-a3898f81` verifies rejection/recovery with `all-null=1`, no
allocation leak, and owner recreation using the rebuilt clean ICD.

## Application evidence and limits

The ordinary small-window run uses the same installed driver and Blender as
the [original failure](blender-turnip-redmi.md), adding only the staged layer.
The separate below-layer capture contains 389 ordinary rendering begins,
successful subsequent submit/wait/present calls, and no rendering-inspector
sequence findings. Physical screenshots retain the visibly incomplete UI;
successful Vulkan returns do not replace that failed application gate.

Artifacts are under Arlinux `build/blender-vulkan/`: `rendering-layer-*` build
and X300 logs, `redmi-rendering-layer*` application logs/maps/capture/analysis,
and `redmi-layer-{probes,owners,dispatch}/` per-case logs and exact commands.
Only isolated layer directories under the Redmi app's `files/run` were
staged. X300's installed Blender/runtime and the user model were preserved.

Active core-alias/linked rendering, multi-physical-device groups, full
secondary/resolve/attachment-extension behavior, allocation failure in the
greater-than-eight-color temporary array, and concurrent active recording
remain unverified. The lifecycle check covers two active devices but does not
establish all concurrent destruction semantics. The independent probe covers
color LOAD preservation, not full depth/stencil or multisample resolve
correctness. No host-memory emulation or shader rewriting has been migrated
to the layer. Remaining white/missing content, full-size acquire handling,
model interaction/export, and the separate OnePlus failure still need work.
