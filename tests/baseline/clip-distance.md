# Clip-distance compatibility

Mali reports `shaderClipDistance=false`. Blender 4.3 `VKBackend::is_supported`
requires that bit. The ICD does the Vortek-shaped automatic path: advertise
the feature, strip it from `vkCreateDevice` before the HAL, and rewrite
used `ClipDistance` outputs to a user Location plus fragment `OpKill`.

No application environment variable. Emulation runs only when the physical
device lacks the feature. `HYBRIS_VULKAN_COMPAT_CLIP_DISTANCE=0` is not
required and is not a product switch.

Host rewrite check: `clip_rewrite_check.c` with `generate-clip.py`.
Device diagnostic: probe mode `blender-vk` prints `BLENDER_VK_REQ` against
the Blender 4.3 minimum list.
Pipeline pNext (dynamic rendering) does not skip the rewrite; only extra
shader stages, stage pNext, or pipeline libraries do. Unused clip/cull
declarations are stripped before the HAL module is created. Fragment
rewrite reuses existing `OpTypeBool` and inserts the clip test after
function `OpVariable`s in the first block.


## Blender investigation, 2026-09-09

The X300 product had the current ICD but an older `libhybris-common.so.1`
(build-id `76a53de13ad0ee13083078669bb067da5d8b1965`). Its known-build Mali
MMUD hook still required an explicit switch. The same first-pipeline shader
pair compiled through the current isolated runtime, while the product
runtime crashed. Replacing only common with the current automatic-hook
build allowed that pipeline to compile without a compatibility switch.
Deploy the ICD and its common/linker dependencies as one coherent build.

The next crash was in the Mali shader compiler on Blender's third graphics
pipeline. The fragment rewrite split the entry block but left an existing
`OpPhi` incoming predecessor pointing at the original entry label. The
original shader passed `spirv-val`; the rewritten one failed its predecessor
check. The rewrite now retargets those predecessor operands to the final
clip continuation block, leaving value IDs unchanged. In the captured
third fragment, the only resulting binary difference is word 363:
predecessor ID 5 becomes ID 127. The corrected module passes validation.

Evidence is retained in the Arlinux parent workspace at
`build/mali-pipeline-investigation/20260909T101306/`:

- `phi-fix-build.log`: full AArch64 build and runtime manifest validation.
- `blender-phi-fix-dump-104151/`: 128 original/converted module pairs, all
  256 modules passing `spirv-val --target-env vulkan1.2`; JSON validator
  results, shader files, runtime manifest, loaded maps and device screenshot.
- `blender-phi-fix-104040/`: real Blender 4.3.2 reaches its Python timer,
  submits/presents and exits normally after the correction.

**Rendering acceptance remains failed.** The Blender content is black,
with Mali queue faults (and a tiler heap OOM in one run), despite successful
Vulkan return codes. Timer execution and a clean exit prove startup progress,
not a working UI. These runs use isolated libraries; they do not claim the
installed product libraries have been updated. No WSI changes were made as
part of this investigation.

Follow-up runs `blender-phi-fix-normal-104321` (without `--debug-gpu`) and
`blender-phi-fix-sync-104426` (SyncVal explicitly confirmed active, no shader
printf) still show GPU faults. The SyncVal run reports no VUID or
synchronization errors; this does not rule out errors introduced below the
layer by the ICD or driver. `blender-phi-fix-small-104546` uses an 800x600
window and additionally saves Blender's own GPU screenshot as `client.png`.
That screenshot shows only a partially drawn UI, while Android's screenshot
shows a gray client area. Queue faults and GPU timeouts persist, so the
failure is not limited to a large window or Android screenshot capture.


## Unused clip planning and later application evidence

The font vertex module declared ClipDistance without writing it. Module
creation removed this unused built-in, but pipeline planning still planned a
clip varying from the original declaration. The resulting fragment kill read
an unwritten location. Planning now first applies the same unused-built-in
cleanup. This is separate from the `OpPhi` predecessor correction above.

The existing six-distance shader rewrite check still passes. An audit of the
64 original vertex/fragment pairs from `blender-phi-fix-dump-104151` now yields
64 zero-distance plans and no planning errors; results are retained in
`clip-unused-plan-audit.json`. No new unit test was introduced. This audit
alone does not prove rendering correctness for shaders that use clip distances.

Further diagnosis found broken rendering suspension chains and missing host
flushes in Blender's texture/immediate uploads. Scoped handling now produces
the default scene, splash, text and toolbar, but repeated updates still report
Mali GPU timeouts. The old black-frame observations above remain historical
failures; current results and open acceptance limits are in
[Blender Vulkan compatibility](../../docs/blender-vulkan-compat.md).
