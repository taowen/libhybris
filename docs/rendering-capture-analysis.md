# Dynamic-rendering capture analysis

`tools/inspect-rendering-capture.py` reads the standard JSONL produced by
`gfxrecon-convert`. It does not introduce a capture format or intercept the
application. It checks dynamic-rendering begin/end/suspend/resume sequences
inside each submitted batch and associates findings with the recorded command
buffer generation, attachment creation, and draw binding call indices.

```sh
gfxrecon-convert --format jsonl --output calls.jsonl application.gfxr
python3 tools/inspect-rendering-capture.py calls.jsonl \
  --registry tests/baseline/build/validation-build/src/Vulkan-Headers/registry/vk.xml \
  --output rendering-analysis.json
```

Use the pinned converter and registry from the actual tool build. The report
retains input and registry SHA-256 hashes. Command action/state/synchronization
categories, including aliases, come from that registry. Exit 1 means findings,
2 means inspection failed, and 0 means no findings **within this scope**.
The analysis runs on the host; an AArch64 converter can run on the device with
its staged glibc loader and dependencies, as the existing window capture runner
does. Preserve the original `.gfxr`, converter log, JSONL, application log,
runtime manifest and loaded-library maps together.

For each submitted rendering segment the report includes:

- Its begin/end call indices, flags, rendering parameters and attachment
  view/image creation records when available.
- Draw arguments and the preceding graphics pipeline, descriptor-set,
  vertex/index-buffer binding calls. Push-constant write ranges retain their
  originating call indices, including overlapping writes.
- The suspension/resumption indices and intervening action/synchronization
  calls, so a fault reported at a later fence can be traced back to a batch.

These are binding **call references**, not proof of shader-visible values.
Descriptor contents, descriptor-layout compatibility, indirect arguments,
push-constant bytes, resource lifetime and memory visibility still require
the source JSONL, resource dumps and validation. Secondary command buffers
are not expanded. Unknown commands, unhandled binding commands and missing
recordings appear under `uncovered`; findings involving those streams are
marked partial. Zero findings do not prove correct pixels or complete Vulkan
validity. Swapchain image creation may be absent from the image table because
those images are returned by enumeration rather than `vkCreateImage`.

## X300 Blender evidence, 2026-09-09

Blender 4.3.2 on `10AFA31610002QH` reached submission after repairing the
ClipDistance fragment rewrite's `OpPhi` predecessor. Its UI was still black
or incomplete, with Mali queue faults, timeouts and occasional tiler heap OOM.
An upstream VVL/SyncVal run did not diagnose the sequence below.

The standard GFXReconstruct layer captured frames 1–2 with unassisted memory
tracking. The original failure still occurred during capture. Evidence in the
parent Ardesk workspace is under
`build/mali-pipeline-investigation/20260909T101306/blender-phi-fix-capture-104859/`:
`capture.tar`, `conversion/calls.jsonl`, `rendering-analysis.json`, the runtime
manifest, loaded maps and application log. This is a real application capture,
not the independent pipeline-compilation probe used earlier in the investigation.

The inspector identifies 17 suspension/resumption pairs containing forbidden
commands and 96 resumes without a corresponding suspension in the known
command stream. The first affected batch is `vkQueueSubmit` index 3092:

| Call index | Operation |
|---|---|
| 1476 | Begin command buffer 747 |
| 1481 | Begin rendering with `SUSPENDING`, attachment view 178 / image 176 |
| 1483 | End rendering |
| 1484–1488 | Barriers, buffer-to-image copy, buffer copy |
| 1489 | Begin rendering with `RESUMING` |
| 1494 | Draw using pipeline 189, descriptor set 190 and vertex buffer 183 |

[Vulkan's dynamic-rendering rules](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#VkRenderingFlagBits)
forbid action or synchronization commands between suspension and resumption.
The captured sequence also agrees with the flag assignments and transfer-group
handling in [Blender 4.3.2's command builder](https://github.com/blender/blender/blob/v4.3.2/source/blender/gpu/vulkan/render_graph/vk_command_builder.cc).
Thus a successful submit/fence return and no upstream validation message did
not establish a valid command sequence.

An isolated ICD diagnostic copied `VkRenderingInfo`, cleared suspension/resume
bits only for LOAD/STORE attachments without resolves or attachment extensions,
and added a memory dependency before resumed segments. Run
`blender-phi-fix-small-105603` rendered the startup UI and exited normally,
without the original queue faults in that bounded run, but retained image
stripes and missing icons. Extending the dependency to every eligible begin in
`blender-phi-fix-small-105844` produced a clean startup illustration while icons
were still incomplete; that run also reported a timeout and tiler heap OOM.
Both the client GPU screenshot and Android screenshot are retained. These are
diagnostic comparisons, **not a completed compatibility fix or rendering gate**.
The installed product libraries were not overwritten.

The next compatibility work must preserve attachment load/store/resolve
semantics, scope memory dependencies correctly and retain the same submission
and pixel evidence. Remaining icon, GPU-fault and output-alpha issues stay
open. The temporary single-device diagnostic is not suitable for multi-device
dispatch or general production use.

The inspector was also run against both saved dynamic-widget captures in
`tests/baseline/build/results/20260907T042356-051c616f/capture-dynamic/`.
Both report no sequence findings. The deliberately bad widget still has its
separate data/pixel failure: this check is not a replacement for that gate.
