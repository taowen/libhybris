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

## Generate a draw-resource request

Choose draw indices from the report, then generate the converter's standard
resource request with its command-buffer, render-segment and submit boundaries:

```sh
python3 tools/inspect-rendering-capture.py calls.jsonl \
  --registry tests/baseline/build/validation-build/src/Vulkan-Headers/registry/vk.xml \
  --output rendering-analysis.json \
  --dump-draw 1494 --dump-draw 1505 --dump-draw 4939 \
  --dump-request draw-resources.json
gfxrecon-replay --swapchain offscreen --screenshots 2 \
  --dump-resources draw-resources.json --dump-resources-dir resources application.gfxr
```

Creating a request does not suppress the inspector's exit 1 for sequence
findings. A repeated recording needs `--dump-submit` to select an unambiguous
execution. Missing draws or incomplete primary recordings are rejected.
The generated request enables raw attachments, depth, bound descriptors and
vertex/index-buffer dumps. The indices above belong to the Blender capture
below, not arbitrary captures. Replay still requires a correctly staged loader,
ICD and dependencies; resource readback may alter scheduling and is a diagnostic
comparison, not a substitute for uninstrumented application rendering.

The first manual request omitted `RenderPass` boundaries, and the frozen
GFXReconstruct converter/replayer build `c2ff0ee` crashed before replay. Supplying
the boundaries fixed this configuration error. The generated request was run
on X300 in `replay-blender-resources-111645` under the same parent investigation
directory. The replay's maps identify the frozen diagnostic ICD/common from
`blender-phi-fix-small-105844` and the product's standard Vulkan loader.
The three selected draws produced attachment dumps plus font/splash textures,
font/splash vertex buffers and the widget's 272-byte UBO. That UBO matches the
captured `vkCmdUpdateBuffer` at index 1477 byte for byte; this is GPU buffer
readback, not proof of the shader's effective UBO reads. The no-vertex-attribute
widget's index buffer was still absent from this tool build's report and remains
a diagnostic gap. These replay records use the identified frozen tool build,
not the newer revision currently selected by `build-capture-tools.sh`.

A subsequent fork fix (`bac419e7`, now pinned by `build-capture-tools.sh`) copies
indexed-draw state before the no-vertex-attribute early return. The X300 replay
`replay-blender-index-fixed-112611` used the same capture and frozen diagnostic
ICD with newly staged, manifest-checked tools. It completed both frames with
exit 0 and reported buffer 196 at draw 1505: UINT16, offset 0, 36 bytes.
The binary contains eighteen zero indices. This closes the missing-resource
reporting gap, but does not establish correct application data: captured
writes, replay memory restoration and readback still need comparison. Font and
splash attachment/texture dumps match the preceding replay byte for byte;
the widget attachment differs. Neither successful replay nor an available
buffer dump is a rendering pass.

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
