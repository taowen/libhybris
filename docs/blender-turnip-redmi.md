# Redmi Blender / product Turnip diagnosis — 2026-09-09

Application acceptance is **FAIL**. This is Redmi `29854870`, M2012K11AC,
Adreno 650, not the separate OnePlus / Adreno 830 device-lost investigation.
X300's successful bounded Blender workflow does not establish this backend's
compatibility. No Turnip, hybris runtime or Blender binary was changed here.

## Runtime and launch

The installed home launcher starts Debian Blender 4.3.2. The explicit
`VK_DRIVER_FILES` selects the product `freedreno_icd.json`. Live mappings
confirm `usr/lib/mesa/libvulkan.so.1` and `libvulkan_freedreno.so`; this is the
glibc Turnip path, without the hybris ICD. The driver reports Mesa 26.3.0-devel.
The actual ELF identity, rather than the current Mesa checkout, identifies
the installed runtime:

| File | SHA-256 |
| --- | --- |
| Blender | `4b8538be4e595b6150bf4a205c5b74b77e174c3a26d6f96cac7f326ca28899d7` |
| Vulkan loader | `22b6ce3145007566e3e47ce1f379135ba9b83e734cba2edc2d0c3e1b28ffeea7` |
| Turnip | `0ae23ffbc4edf1663eacbfa2d74d35e1e1b2f0a131127af9906fb9df382419e7` |
| bionicx runtime | `740aee3c40d8c2be8536402b9a976ff91f25bb073cd65e6a5011d420b5aef620` |
| Staged validation layer | `ad1587bed5a334930a48dbb04afca1a630ad7da012ea92cb8e17a53ce18f6934` |
| Staged capture layer | `16a9fa6dee6cc5c5d85ea8f499751b844b968b643400c3e1a4807a2d65c934d0` |

Turnip build-id is `1c3481ab402aa9012667a8cfba157325436874f6`.
Symbolication used the library pulled from the device; the local Mesa build
has a different hash and must not be used as its symbol/source provenance.

The first manually reconstructed product launch failed to locate
`libpulsecommon-17.0.so`. The library exists under
`rootfs/usr/lib/aarch64-linux-gnu/pulseaudio`; adding that directory to this
launch's `LD_LIBRARY_PATH` permits startup. This is a diagnostic environment
adjustment, not a verified fix to the normal terminal launcher or loader.
All subsequent runs retain it and use `--factory-startup --gpu-backend vulkan
-noaudio`. The device home and installed binaries remain intact.

## Full-size startup: waiting after failed acquire

The first run with `--debug-gpu` identifies Turnip, then remains before the
Blender window appears. A read-only debugger attachment finds the main thread
in `kgsl_syncobj_wait` / `vk_common_WaitForFences`. The process was detached
after collecting all thread stacks, then terminated before the next run.

A separate capture without `--debug-gpu` records:

- Surface capabilities and swapchain creation at 2320 x 1080.
- An unsignaled acquire fence, capture ID 14, created at call 30.
- Earlier queue submits and waits returning success.
- Call 205: `vkAcquireNextImageKHR`, fence 14, returns
  `VK_ERROR_OUT_OF_DATE_KHR`. No later call is captured while the process waits.

The workspace's Blender 4.3.2 source at `32f5fdc` corroborates the caller:
`intern/ghost/intern/GHOST_ContextVK.cc:548` ignores acquire's return value,
then waits indefinitely for that fence. It only handles OUT_OF_DATE after
presentation. A failed acquire is not evidence that the fence has been
signaled. This explains the wait without establishing a GPU execution hang.
The precise initial resize/configure transition remains to be captured;
do not silently return success or signal a failed acquire's fence to hide it.

## Small-window startup: dynamic-rendering crash

Adding `-p 100 100 800 600` passes the first acquire but exits 139. The ordinary
run and a separate capture run both crash. The actual installed Turnip resolves
the ordinary crash stack to:

```text
tu_cs_add_entries
tu_append_pre_chain
tu_insert_dynamic_cmdbufs
queue_submit
vk_queue_submit_final / vk_queue_flush / vk_device_flush
vk_common_QueueSubmit
```

The flushed capture ends after a complete command-buffer recording and memory
snapshots, before the crashing QueueSubmit returns and is captured. It must
not be treated as a complete replay or supplied with an invented submit.
Command buffer 732, begin 1371 / end 2983, contains 113 render segments:

| Call | Recorded operation |
| --- | --- |
| 1376 / 1378 | Begin SUSPENDING on view 176, then EndRendering |
| 1379–1383 | Barriers and buffer/image copies |
| 1384 / 1390 | Begin RESUMING, then EndRendering without SUSPENDING |
| 1396 / 1405 | Begin RESUMING again, then EndRendering |
| 1411 | Another RESUMING begin |

[Dynamic-rendering rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingFlagBits.html)
exclude action/synchronization commands between suspension and resumption;
a resume refers to an earlier suspended rendering instance. The capture
contains the same class of interrupted/unmatched chains seen on X300. It
does not establish a Turnip defect for valid application input, nor prove
that an X300 workaround is sufficient for Redmi.

The reused rendering inspector now reports 13 interrupted chains and 81
unmatched resumes in this recording, separately from the seven captured
submission batches with no findings. It labels the missing submission context
and refuses to generate a draw-resource request for this recording.

Standard validation was staged separately and loaded in the Blender process.
With `--debug-gpu`, its startup diagnostics confirm DebugPrintf and additional
device features; that run also crashes, without a corresponding validation
error in its log. This is not a SyncVal pass or a validity certificate.
Ordinary and captured runs without DebugPrintf independently reproduce the
crash, so DebugPrintf is not required to trigger it.

## Shared capabilities and next implementation boundary

The standard loader, validation/capture tools, JSONL converter and rendering
inspector already work with Turnip. They do not require nesting Turnip inside
the Android HAL bridge. The new real capture supplies a concrete candidate
for shared compatibility: the existing rendering-segment transformation.

Before moving it above both ICDs, preserve its load/store/resolve behavior,
command dependencies, per-device dispatch and failure handling. Keep driver
policy separate from the reusable transformation. Redmi's captured extension
set additionally contains `VK_EXT_shader_stencil_export`; the current hybris
application whitelist would reject it. Merely reusing the application name
or copying all Mali policy bits is therefore insufficient. Noncoherent
upload/readback fixes must remain conditioned on actual memory properties.

The acquire-result problem needs its own application/WSI treatment. A small
window is only an isolation control. Full-size startup, resize, interaction,
render/export and the original OnePlus failure remain open.

## Artifacts and tool verification

Local ignored artifacts are under Ardesk `build/blender-vulkan/`:
`redmi-product-{baseline,validation,capture,small-window,small-capture,small-debug-validation}.log`,
`redmi-product-baseline-stacks.txt`, baseline/validation `.maps`,
`redmi-product-small-window.crash.txt`, `redmi-product-identities.txt`,
`redmi-product-{startup,small}.gfxr`, converted JSONL and
`redmi-product-small-rendering-analysis.json`.

- Startup capture SHA-256:
  `dc919a7ac982f2f9a77913096c42e95fd22c3fbd62c1f6f3d05df04773714f6d`.
- Small-window capture SHA-256:
  `0d7774064ee3f25d72abc979f91c295c352d4dc62a1d3d57db9c0030e6094488`.
- Small-window JSONL SHA-256:
  `a6cc5b6789805257cfc9938e1fddabb453bfc353f847dd5dcd2be199629894f8`.

The modified Python tool passes byte compilation. Actual Redmi captures
verify the new recording-only report and rejection of an uncaptured-submit
draw request. The saved X300 startup capture `135218` retains identical
submission reports, finding counts and draw-resource requests for
4860/4868/4892 compared with the pre-change inspector. No native source was
changed or native rebuild claimed for this diagnostic-tool batch.
Both saved dynamic-widget captures from `20260907T042356-051c616f` still
report no sequence findings and exit 0; the bad widget's separate pixel/data
failure is unaffected. The real Redmi recording-only report exits 1 with
one uncovered submission context, not a false all-covered result.
