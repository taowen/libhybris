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
converted calls remain available for further analysis. Desktop replay is an
explicit option, with the bounded comparison and current failures described below.

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

### Default descriptor-buffer replay remains failed

A separate manual replay of Mali capture `20260908T164330-203aa6db` used the
same staged runtime and pinned replay tool. `--swapchain virtual` exited 255
because automatic WSI selection had no compositor. After that process ended,
`--swapchain offscreen` reached API call 451, `vkCreateImageView`, but returned
`VK_ERROR_OUT_OF_DEVICE_MEMORY` instead of the recorded `VK_SUCCESS` and exited
255. No draw was reached. Commands and both logs are retained under that run's
`capture/replay-attempt/`. The failure's cause remains unproven. The default descriptor-buffer path and
GPU argument verification are not covered by the successful capture checks.

### Fixed-probe readback replay

Use `--replay-capture` with `--capture-tools`. `--replay-memory none|rebind`
selects the upstream tool's allocator mode; it defaults to `none`. A separate
`--zink-descriptors auto|lazy|db` option passes the upstream Zink setting to the
probe and records it in the command. It does not change the default or patch
Mesa. The tested Mali diagnostic configuration adds:

```sh
--zink-descriptors lazy --replay-capture --replay-memory rebind
```

The replay helper writes the standard tool's `replay-request.json`, requesting
each captured `vkCmdCopyImageToBuffer` by its command-buffer begin, copy and
successful submit indices. It checks the fixed fixture's tightly packed 16x16
RGBA/BGRA layout, compares the tool report's resource IDs and submission set,
and compares each retained GL image byte-for-byte. Repeated cumulative report
entries must agree. Output files are saved with device-verified hashes in
`replay-files.json`; commands, logs and per-image comparisons are retained in
`replay-result.json`, `replay.log` and `replay-readbacks.json`.

The original twelve packed draws check their pixels but do not save image
files. Their replay readbacks are retained and explicitly marked
`NOT_SAVED_BY_PROBE`; they are not counted as image comparisons. This is the
fixed fixture's transfer readback evidence, not arbitrary draw/attachment
history, indirect GPU argument inspection, present replay or screen capture.

`render_status` is determined by the original probe and host image checks even
when capture/replay fails. `capture.status`, `capture.replay.status` and aggregate
`status` remain separate. A matching replay of a failed original image does not
make rendering pass. Tool warnings/errors and GPU faults prevent a replay PASS;
when the tool completes, image comparisons are retained even on that failure.

The unpatched fixed GFXReconstruct skipped an original submit with no command
buffers while dumping resources. Zink uses a trailing empty submit to signal
its timeline semaphore. The missing signal blocked the next wait: manual lazy
capture `20260908T171021-c9642933` completed replay without resource dumping,
but timed out after 60 seconds with dumping, leaving only its first readback.
The [fork commit](https://github.com/taowen/gfxreconstruct/commit/e6865bedad471a7ffaada05b6b18b29d77da36ee) preserves that original
submit's semaphore/fence work. Replaying the **same capture** with the patched
tool completed all 27 readbacks and matched all 15 saved images. This comparison
used the pre-consolidation runtime saved with that capture; subsequent runs use
libhybris `734ec73`, built with Ardesk's shared protocol package.

Using `-m rebind` on the original descriptor-buffer capture is not a solution:
the pinned tool explicitly warns that this mode is unsupported and Mali logged
`GROUP_ERROR_FATAL` despite process exit 0. The runner rejects this combination
when the capture contains descriptor-buffer commands. The separate lazy-mode
capture preserves the eight original Mali attribute failures.

Final device evidence is recorded below. Every listed expanded capture has
four CPU-upload candidates and fully saved converter sidecars. None establishes
GPU argument contents or closes G04/G06/G10/G12.

| Final run | Rendering | Replay | Retained image comparison |
| --- | --- | --- | --- |
| `20260908T173722-a71cc7ea` | Mali basic fixture PASS, explicit lazy mode | PASS, rebind; 13 readbacks | Main image matches; 12 packed images not saved by probe |
| `20260908T173400-11780a2d` | Mali expanded fixture FAIL, explicit lazy mode | PASS, rebind; 27 readbacks | All 15 saved images match, including eight attribute failures; 12 packed images not saved |
| `20260908T173401-b1104260` | Turnip expanded fixture PASS, explicit lazy mode | FAIL, no memory translation; 50 readbacks | 37/38 saved images match; `multidraw-0.rgba` differs; 12 packed images not saved |
| `20260908T173550-4d8ada8d` | Mali basic fixture PASS, default descriptor mode | FAIL, no memory translation; exit 255 at image-view creation | No replay comparison completed |

Turnip also mismatched the same multidraw image with `-m rebind`
(`20260908T172622-4e3a0435`), alongside a missing image-layout warning. The
no-translation run still reports the tool's removal of the pipeline
compile-required flag. These are retained failures, not evidence of an
unchanged Turnip application image. The root cause of that pixel divergence
remains open.

The patched tool also passes the existing independent Adreno HAL widget run
`20260908T173058-6d4246a2`: version, UBO and all ordinary/dynamic/multiple
correct-versus-wrong binding capture/replay pairs pass (5 PASS). The tools
were built with the fixed patch, then rebuilt through the clean-source patch
application path; every installed file remained byte-identical. The final
libhybris clean build used `734ec73` and
`ARDESK_WSI_PROTOCOL_DIR=/var/home/taowen/projects/glibc-on-bionic/ardesk/protocols`.
Shell syntax, Python compilation and actual helper hashes recorded in the
final runs were checked. Earlier manual commands, failed logs and partial
outputs remain under the source captures' `capture/replay-attempts/`.


The current builder uses `taowen/gfxreconstruct` at
`e6865bedad471a7ffaada05b6b18b29d77da36ee` directly. The comparisons above
predate the fork migration and retain their original build identities.
The empty-submit change is now a normal fork commit; the builder applies no
patch. New manifests record `source-repository.txt` and `source-revision.txt`
alongside the actual source-tree and installed-file hashes.
