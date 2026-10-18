# Product stack consolidation checklist (2026-09-09)

This checklist follows the current source tree. Mesa is pinned to
`bfe5f4ceb762504532ccdd7f19c1c2cdd31791b4`.
"Consolidated" means the corresponding implementation or entry point has been
unified; it does not mean that the entire WSI or application acceptance passed.

## Consolidated

| Item | Current implementation |
| --- | --- |
| Protocol definitions | Arlinux's `protocols/` package, `arlinux-wsi-protocols`, contains TAWC-DRI 0.4 and `android_wlegl`; hybris no longer ships its own XML. |
| Xwayland | Built only by Arlinux's `third_party/xwayland`; hybris builds standalone clients. The runner attaches by default to the installed, running `io.taowen.arlinux.debian`. |
| Compositor entry | The Arlinux anlabwc scene consumes AHBs. Client tools no longer build the old test APK or their own Xwayland. |
| Product Mesa source | `arlinux-wsi` in `taowen/mesa`, pinned to `bfe5f4ce`; no WSI patch is applied during the build. |
| Mesa probe builds | desktop-gl invokes the product's `tools/build/mesa.sh`, packages the product libraries directly, and records the product commit, source-tree checksum, and protocol checksum. There is no second independently maintained pin. |
| Window validation entry | `tests/wsi/run.py --backend hybris/turnip` shares clients, validation, capture/replay, screenshots, and X11 release checks. Turnip reuses the product Mesa runtime; see the [evidence](../tests/wsi/product-backends.md). |
| Frontend window removal | EGL Wayland/X11 plugins and PRESENT_SOCKET have been removed, as have the Vulkan frontend plugin layer, Gladio/Vortek GPU hosts, and in-APK shm demo clients. Window runners use only the standard loader and ICD; native-window lives in `vulkan/icd/`. Old overlay directories `usr/lib/{gladio,vortek}` are removed only during installation. |
| Submodule pointers | Parent gitlinks follow verified, pushed libhybris commits. The gitlink identifies the exact version; updating it does not replace product reinstallation. |
| Tool customization ownership | SPIRV-Tools and GFXReconstruct customizations are committed in their respective forks. Builders use full commits and record source and output hashes. |

## Intentionally separate backends

| Item | Current state and remaining acceptance |
| --- | --- |
| Product GPU backends | Standard loader → Turnip WSI for Adreno; standard loader → hybris ICD WSI for Mali/vendor HAL. They do not need to become one DSO. |
| Import | ICD `wsi.c:image_limits` queries AHB external-image support and requires IMPORTABLE before filtering formats and usage. Turnip's `wsi_common_arlinux_formats.c` first queries import capability for a linear DMA-BUF image, or a DMA-BUF buffer plus GPU copy, then filters formats, usage, and extent. Creation still validates actual FDs, layout, size, and memory types. See [queries and device coverage](../tests/wsi/product-backends.md#dma-buf-import-capability-gate--2026-09-08). |
| FIFO/release | Both product paths support FIFO and reuse driven by actual release. Shared gates cover acquire timeouts and surface-lost after native X-window destruction. Disconnects, delayed release, and concurrent destruction races still require acceptance. |
| Usage/alpha/extent | Turnip preserves opaque alpha and derives usage/extent from queries for the importable path. ICD `wsi.c` also advertises and conveys opaque alpha, deriving usage/extent from HAL import queries. Do not mechanically make different values equal: validate compositor consumption semantics and each import path individually. |

## Remaining consolidation or acceptance

| Item | Confirmed remaining differences |
| --- | --- |
| Window validation/capture | Both Vulkan backends use the shared window gate. Native scenes and the product teapot share one Host (`tests/device_gate.py`, `Host.deploy`/`pull_maps`). The guest-desk plus `TEAPOT_ONCE` bypass in `tests/test-wsi-buffer-layout-device.sh` was deleted. Applications have VVL and capture before/after compatibility transforms. The Mali startup shader-block validation error across stages was fixed; downstream VVL reports zero errors on both backends. Complete application interaction and replay remain unverified. desktop-gl remains offscreen and cannot replace application-window acceptance. |
| Historical documentation | `TAWC_FORK.md` marks the removed Vulkan/EGL window plugins and old packaging entries as retired. `integration-review.md` separates current entries from old fixture records. Historical APK hashes and run IDs are retained, but are not evidence for the current product gate. |

## Build boundaries and the Mali quirk

A standalone libhybris checkout points `ARLINUX_WSI_PROTOCOL_DIR` at the shared
protocol package. When embedded in Arlinux, the builder defaults to
`../../protocols`. Product packaging and installation select existing hybris
outputs through `HYBRIS_LIB_DIR`; do not duplicate the protocol XML.

MMUD handles HAL initialization compatibility. It is neither a protocol field
nor a WSI capability. With `--enable-mali-quirks`, the current build automatically
enables an in-process hook for `libGLES_mali.so` build-id
`5ac4efe8d6175298b273dbaeb8f9d28e5e508e72`.
`HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK=0` disables it; `1` explicitly enables it.
Other build-ids and secure execution do not use this hook. Distinguish enabling
Mali quirks at build time from automatic runtime matching; the current behavior
is not purely runtime opt-in. See the [initialization evidence](mali-mmud.md).

## Per-item acceptance boundaries

Shared window cases verify FIFO presentation, reuse after actual release,
zero/finite acquire timeouts, resize, and surface-lost after native X-window
destruction on both backends. They do not cover delayed release, disconnection,
or concurrent destruction, and do not close the entire contract.

Shared Wayland color frames cover opaque composition with alpha=0 on both
devices. They do not cover every format, mixed alpha, or window lifetime. Usage
requires real image operations for each advertised bit, rather than just
requesting the bit in createInfo. Resize evidence does not cover every extent
boundary or creation race.

The product Zink teapot and native AHB scenes in `tests/test-scene-ahb-device.py`
are different workloads sharing one Host. A scene is not a Vulkan client;
setting VVL/GFXReconstruct variables does not turn it into a window probe.
Product pixel checks validate teapot presentation and resize. Khronos-layer
probe validation and application diagnosis are recorded separately. Application
capture reuses deployment and file retention, but capture without pixel
comparison is not a successful replay. Offscreen `desktop-gl` results cannot
replace either application-window gate.

Delayed release, disconnection, concurrent destruction, real advertised-usage
operations, other alpha combinations, and missing-protocol behavior without
TAWC-DRI require broader coverage, not another consolidated entry point.
`swapchain-review` continues to use only the hybris allocation hook.

## Compatibility and application diagnostics consolidation (in progress)

Standard loader → `VK_LAYER_HYBRIS_compat` → selected ICD. One shared layer
implements generic device policy, format filtering, shader rewriting, BC upload,
command state, and rendering compatibility. The HAL ICD retains Android loading,
queues, and WSI; Turnip uses the product Mesa ICD. The layer does not depend on
Android HAL or select a second compatibility implementation by Turnip driver ID.
Actual format and feature capabilities still determine policy on each GPU.

`tools/vulkan_backend.py` and `tools/vulkan_layers.py` share output-hash checks
and layer deployment between baseline probes, window tests, and
`tools/run-vulkan-app.py`. The application entry reuses the window Host and
Diagnostics, recording program/runtime-interposer hashes, actual mappings,
screenshots, and bounded logs. PID plus start time bind diagnosis and cleanup;
applications may change directories. Capture may occur before or after
compatibility conversion. Raw application captures are retained without being
counted as successful pixel replay. `OBSERVED` only means diagnostic observation
finished; interaction/save/resize acceptance remains unverified.

This consolidation was packaged and installed on both devices; product
deployment records are in Arlinux's `docs/vulkan-compatibility.md`. Subsequent
Features2 request fixes were also reinstalled. Both product GLX/Wayland teapots
passed presentation and resize pixel gates; see the [device-feature chain
evidence](../tests/baseline/device-features.md).

Current device records from isolated build `/tmp/libhybris-unified-build`:
Mali forced compatibility and synchronization validation passed. Turnip passed
seven forced-compatibility synchronization cases, but the BC image case still
failed: 384 readbacks, 12,524 comparison errors, and zero VVL errors. The native
Turnip control also had 11,912 reference-image filtering errors; BC versus
reference-image comparison errors were 131,158, reduced to 612 with compatibility.
Both runs remain failures. Attribution among the driver, reference filtering,
and missing RGB alpha-boundary semantics is incomplete.

Application diagnosis with Blender fixed ELF `83916197…`: Turnip run
`20260909T194146-d2deb5fe` loaded VVL, requested synchronization validation, and
recorded zero error markers. Mali `20260909T195324-9c5ecb6a` failed with
`VUID-RuntimeSpirv-OpVariable-08746`: VS output BuiltIn blocks did not match GS
inputs. Explicit Mali HAL loading uses the exported sphal namespace. Observation
`20260909T195202-dfd75d62` displayed the complete startup UI, but logs still warn
about mapper/GLES loading in the default namespace; not every HAL namespace call
has been resolved.

Automatic shader preparation was subsequently changed to preserve interface
declarations, fixing that startup VS/GS error. Mali
`20260909T215607-7621e2c7` and Turnip `20260909T215651-e223bb44` both report zero
downstream VVL errors and warnings. Product teapot gates passed on both devices
after reinstalling the new product. The [complete evidence](blender-shader-interfaces.md)
retains the original failing control and unverified items.

Raw before/after application captures were retained and their device hashes
checked: Turnip `20260909T193430-33a55cb5` / `20260909T193340-df2e0080`; Mali
`20260909T195941-73551413` / `20260909T195837-69a75e7d`. These establish capture
availability, not successful application replay or pixel equivalence.

Shared Wayland synchronization validation after consolidation passed on Turnip
`20260909T194518-89614e6d` and Mali `20260909T195604-6e6e51b5`. Turnip additionally
passed memory-ranges, blender-readback, life, vk-init, ubo-pool-reset, and
render-segments. Raw Android ABI/HAL and explicit raw-ICD cases are excluded from
the default Turnip set with reasons in not_applicable_cases. Explicit selection
of an inapplicable case fails rather than counting as a pass.

The pinned gfxrecon-convert parsed all four application captures successfully;
each contains nine vkQueuePresentKHR calls. The before/after positions also
show real API differences: Turnip has 1948/2509 vkCmdPipelineBarrier calls and
Mali 1949/2510. Mali vkCreateDevice.shaderClipDistance changes from true on the
application side to false on the driver side. This verifies different capture
boundaries, not successful pixel replay.
