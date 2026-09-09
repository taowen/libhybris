# Check capture memory visibility before blaming a draw

A resource dump describes the replay's data. Compare it with captured host
writes and their flush order before treating a difference as a compatibility
layer or shader failure. Keep the memory tracking and replay allocation modes
in the evidence record; they can change the result even on the same GPU.

Use the standard converter to retain host-write bytes:

```sh
gfxrecon-convert --include-binaries --format jsonl --output calls.jsonl application.gfxr
```

For a buffer consumed by a failing draw, follow `vkBindBufferMemory` to its
allocation and offset, and `vkMapMemory` to the mapped offset. A
`FillMemoryCommand` offset is relative to that mapped pointer. Locate the
covering binary range and compare it with the replay's index/vertex/descriptor
dump. For uploads, also follow `vkCmdCopyBuffer` and its source range. Check
memory property flags and the order of fill, flush and queue submission.

The standard transfer dump can isolate a copy without selecting a draw:

```json
{
  "BeginCommandBuffer": [1476],
  "QueueSubmit": [3092],
  "Transfer": [[1497]],
  "DumpResourcesOptions": {
    "DumpRawImages": true,
    "DumpBeforeCommand": true
  }
}
```

These indices are specific to the capture below. The transfer report's `file`
and `beforeFile` contain the **destination** range after and before the copy;
`beforeFile` is not a dump of the source buffer.

## X300 Blender comparison, 2026-09-09

Evidence is under the parent workspace's
`build/mali-pipeline-investigation/20260909T101306/`. Replays below use the frozen
diagnostic ICD from `blender-phi-fix-small-105844`, with its limited rendering
flag/dependency workaround, and manifest-checked GFXReconstruct `bac419e7`.

The original `blender-phi-fix-capture-104859` used `unassisted` tracking.
Buffer 197 is bound to memory 182 at offset 74048; its 36 bytes are copied to
index buffer 196 by call 1497. Memory 182 has type 1, flags `0x0b` (device local,
host visible, host cached, **not host coherent**). Its captured flush at 311
precedes the first fill for this memory at 3090, immediately before submit 3092.

`replay-blender-binary-convert-112833/calls/145_fill_memory.bin` on the device
contains the full captured allocation; the extracted host artifact
`index-upload.bin` contains the range at offset 74048. Its UINT16 values are:

```text
0 1 2 2 1 3 4 5 6 6 5 7 8 9 10 10 9 11
```

| Replay | Observation |
|---|---|
| `replay-blender-index-fixed-112611` | Draw 1505's 36-byte index dump is all zero |
| `replay-blender-index-transfer-113013` | Copy 1497's destination is zero before and after the copy |
| `replay-blender-widget-only-113106` | Selecting only draw 1505 still reads zero indices |
| `replay-blender-widget-rebind-113158` | `--memory-translation rebind` reads the captured indices exactly; additional icons appear, while image stripes and missing text remain |

The fork's `VulkanCaptureManager::QueueSubmitWriteFillMemoryCmd` writes every
mapped allocation in unassisted mode; its flush hook handles page-guard,
userfaultfd and assisted tracking. `VulkanDefaultAllocator::WriteMappedMemoryRange`
copies restored bytes without flushing them. Thus this capture restores the
non-coherent upload after its recorded flush. The original application's flush
is present; these zero replay indices do not establish an application or ICD
copy defect.

`blender-phi-fix-capture-pageguard-113401` recaptured the application with
standard `page_guard` tracking, the same frozen original application runtime,
and the updated capture tools. The new stream places host writes before their
flushes. The inspector still finds 17 interrupted suspend/resume pairs and 96
unmatched resumes. In `replay-blender-pageguard-dump-113630`, the default memory
allocator reads the correct eighteen indices at the corresponding draw 1592.
The new indices/boundaries come from `draw-pageguard.json`, generated from this
capture, rather than reusing the old call numbers.

Both the original application image and the page-guard replay remain visually
incorrect. `replay-blender-pageguard-plain-113445` has incomplete glyphs/icons
and a missing startup illustration. Page-guard changes mapped-memory handling
and allocation alignment; rebind changes allocation behavior. Neither is a
pixel-equivalent substitute for the original application. These comparisons
establish a replay fidelity limitation and usable index readback, **not** a
completed Blender compatibility or rendering gate.
