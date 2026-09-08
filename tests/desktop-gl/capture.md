# Vulkan capture of desktop GL workloads

Add `--capture-tools /path/to/gfxreconstruct/install` to `run.py`. Use the pinned
install produced by `tools/build-capture-tools.sh`; each installed file is
verified against its manifest before staging. The shared staging helper also
serves the baseline capture workloads. Capture and validation use separate runs.

For the failing Mali attribute workload:

```sh
python3 tests/desktop-gl/run.py \
  --serial 10AFA31610002QH --hal /vendor/lib64/hw/vulkan.mali.so \
  --api-version 1.3.305 --mali-loader-quirk --packed-vertex 1 \
  --profile core33 --vertex-prepass --vertex-draws \
  --capture-tools tests/baseline/build/gfxreconstruct/install
```

The runner records through the standard Vulkan loader, above the ICD. It saves
`capture/desktop.gfxr` even when rendering fails, checks its SHA-256 against the
device file, and converts it to JSONL with the pinned tool. `capture/commands.json`
retains the conversion command and exit status; `convert.log` retains errors.
The converter's `calls/*.bin` sidecars are also saved, with every file's SHA-256
checked against the device and recorded in `converted-files.json`. Every JSONL
reference must resolve to a saved file. Earlier captures retained the complete
raw `.gfxr`, but their converted binary sidecars were not copied from the device;
those can be recovered by converting the raw capture again.
The GFXReconstruct layer must appear in the process mappings. Converter and
probe cleanup are restricted to their PIDs in the unique run directory.

`capture/vertex-draws.json` indexes recorded draw commands by command-buffer
begin, bound pipeline/create call, vertex attributes, buffers, offsets, effective
strides and divisors. Successful queue-submit calls are linked to the recorded
commands. These references prove submission, not GPU completion or correct
pixels. The original GL image and error checks still decide rendering status.
`probe_exit_code`, aggregate `status`, `capture.status` and
`capture.vertex_analysis_status` remain separate in `result.json`.

`capture/indirect-uploads.json` follows each submitted indirect-count draw to a
preceding `vkCmdCopyBuffer` in the same submission, the source buffer's recorded
memory binding, and its latest overlapping captured CPU upload. A single copy
and upload must cover the full `maxDrawCount` argument range; otherwise the
record is `UNAVAILABLE`. It retains call indices, source offsets, sidecar path
and a hash of that range, and decodes `candidate_commands` using the recorded
stride (including signed indexed `vertexOffset`). A recorded count-buffer fill
is reported only as initialization, never as the actual draw count.

This establishes CPU-upload and recorded-copy provenance. It does not observe
GPU argument contents, exclude shader/aliased writes, resolve cross-queue
ordering, or simulate resource history. The vertex index therefore still marks
indirect GPU arguments as not decoded. Linked pipeline-library state, shader
objects and secondary command-buffer execution also remain unresolved; unknown
linked/shader-object state is marked incomplete. The full standard capture and
converted calls remain available for further analysis. The runner does not
perform desktop replay.

## Device evidence

The AArch64 runtime was built, Python entry points compiled, and the pinned
capture tools' hashes verified on 2026-09-08. Mesa and the GL probe are unchanged.

| Run | Workload | Rendering | Capture / vertex index |
| --- | --- | --- | --- |
| `20260908T164123-49a6acf7` | Mali core 3.3, packed option, main + twelve packed draws | PASS | 1,606 Vulkan calls; 13 recorded/submitted draw commands; vertex state complete |
| `20260908T164330-203aa6db` | Mali, plus explicit compute and expanded vertex draws | FAIL | 3,582 Vulkan calls; 27 recorded/submitted draw commands; vertex state complete |
| `20260908T164331-adf76c1c` | Redmi / Turnip, expanded workload | PASS | 7,262 Vulkan calls; 50 recorded/submitted draw commands; vertex analysis PARTIAL because pipeline libraries are not resolved |

The final Mali capture preserves the same eight attribute failures as the
uncaptured validation run. Four direct commands explicitly combine
firstInstance=5 with instance binding 1/divisor=2: call indices 2508, 2753, 3594
and 3714, corresponding to attribute phases 1, 3, 9 and 10. Their effective
strides are 8, 8, 5 and 8 bytes. This is a combination the
[native KHR property reports unsupported](../baseline/capabilities.md#legacy-divisor-properties-on-khr-only-devices).
The other four failing phases use indirect-count commands. Their CPU-upload
candidates are now decoded below; capturing these calls does not repair them.

The extracted staging helper also passes the independent Adreno baseline run
`20260908T164124-ade5672e`: version, UBO and all three good/bad capture-replay
pairs pass (5 PASS). Those baseline pairs retain exact reference/capture/replay
pixels and descriptor/shader/attachment evidence. Their replay success does not
establish replay support for the desktop captures above.

### Binary preservation and indirect uploads

After another successful AArch64 incremental build, the final capture path ran
on both devices:

| Run | Rendering | Saved / referenced binaries | Indirect uploads |
| --- | --- | --- | --- |
| `20260908T170415-25e94fdc` | Mali FAIL, same eight attribute phases | 259 / 259, device hashes match | 4 FOUND, 0 UNAVAILABLE |
| `20260908T170417-7e571781` | Turnip PASS | 458 / 458, device hashes match | 4 FOUND, 0 UNAVAILABLE |

The runs retain 3,582/7,262 Vulkan calls and 27/50 submitted draw commands,
respectively. Turnip vertex analysis remains PARTIAL because of unresolved
pipeline libraries. All four argument-range hashes match between devices.
On Mali, draw 2889 has one candidate `(vertexCount=3, instanceCount=4,
firstVertex=7, firstInstance=5)`. Draws 3013 and 3264 each have two candidates
`(3, 2, 7, 5)` with stride 32. Indexed draw 3139 has two candidates
`(indexCount=3, instanceCount=2, firstIndex=1, vertexOffset=7, firstInstance=5)`.
Their recorded copy calls are 2851, 2975, 3100 and 3226; captured uploads are
2917, 3041, 3167 and 3292. Each source begins at memory offset 237584. The
count-buffer fill value 3 is initialization evidence only. These candidates do
not prove which commands or count the GPU actually consumed.

### Desktop replay attempt remains failed

A separate manual replay of Mali capture `20260908T164330-203aa6db` used the
same staged runtime and pinned replay tool. `--swapchain virtual` exited 255
because automatic WSI selection had no compositor. After that process ended,
`--swapchain offscreen` reached API call 451, `vkCreateImageView`, but returned
`VK_ERROR_OUT_OF_DEVICE_MEMORY` instead of the recorded `VK_SUCCESS` and exited
255. No draw was reached. Commands and both logs are retained under that run's
`capture/replay-attempt/`. The failure's cause remains unproven; desktop replay
and GPU argument verification are not covered by the successful capture checks.
