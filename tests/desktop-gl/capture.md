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
indirect GPU arguments as not decoded. Pipeline-library vertex input is now
resolved at creation, as described below. Other library state, shader objects
and secondary command-buffer execution remain outside this index; missing or
ambiguous vertex sources are marked incomplete. The full standard capture and
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

Older probe captures checked the twelve packed draws without saving image
files. Those historical replay readbacks remain explicitly marked
`NOT_SAVED_BY_PROBE`; they are not counted as image comparisons. New probes
save and compare all twelve images, as recorded below. This is the
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
libhybris `734ec73`, built with Arlinux's shared protocol package.

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
unchanged Turnip application image. The pixel divergence was subsequently
traced to strided multi-draw capture, as recorded below.

The patched tool also passes the existing independent Adreno HAL widget run
`20260908T173058-6d4246a2`: version, UBO and all ordinary/dynamic/multiple
correct-versus-wrong binding capture/replay pairs pass (5 PASS). The tools
were built with the fixed patch, then rebuilt through the clean-source patch
application path; every installed file remained byte-identical. The final
libhybris clean build used `734ec73` and
`ARLINUX_WSI_PROTOCOL_DIR=/var/home/taowen/projects/glibc-on-bionic/arlinux/protocols`.
Shell syntax, Python compilation and actual helper hashes recorded in the
final runs were checked. Earlier manual commands, failed logs and partial
outputs remain under the source captures' `capture/replay-attempts/`.


The fork migration initially pinned `taowen/gfxreconstruct` at
`e6865bedad471a7ffaada05b6b18b29d77da36ee` directly. The comparisons above
predate the fork migration and retain their original build identities.
The empty-submit change is now a normal fork commit; the builder applies no
patch. New manifests record `source-repository.txt` and `source-revision.txt`
alongside the actual source-tree and installed-file hashes.

### Strided multi-draw capture repair

The multi-draw repair is [fork commit b52a3841](https://github.com/taowen/gfxreconstruct/commit/b52a3841f7669872b85e75435cf90b4d373eb5fa).
The change lives in the fork, with custom encoders and regenerated dispatch
entry declarations; the libhybris builder applies no patch.

In Turnip capture `20260908T173401-b1104260`, call 4124 records four
`vkCmdDrawMultiEXT` draws with a 12-byte application stride. The generic
encoder read consecutive 8-byte structures. It serialized `(3,3), (0,0),
(0,118), (6,3)` instead of the probe's `(3,3), (0,0), (6,3), (9,3)`
(first vertex, vertex count). Replay also used the original 12-byte stride
on the compact decoded array. The last blue band became magenta in
`multidraw-0.rgba`; the original GL rendering passed.

The custom encoders read each logical draw at its application stride and
serialize compact structures with a matching compact stride. Converter JSON
therefore shows the normalized replay stride, not the application's physical
spacing. The driver still receives the original pointer and stride. Indexed
draws with a shared `pVertexOffset` do not read the ignored per-draw offset.
These rules follow the Vulkan [multi-draw](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawMultiEXT.html)
and [indexed multi-draw](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawMultiIndexedEXT.html)
parameter semantics. Old captures with missing draw data require recapture.

The AArch64 capture layer and replay tools were rebuilt using the pinned
builder. Fresh device evidence uses the existing expanded GL probe:

| Run | Original rendering | Replay evidence |
| --- | --- | --- |
| `20260908T184224-9046f986` — Turnip, lazy descriptors, no memory translation | PASS | 50 readbacks; all 38 saved images match, including `multidraw-0.rgba`. Call 4124 contains the four correct draws and stride 8. Replay remains FAIL because the tool warns about removing the pipeline compile-required flag. |
| `20260908T184306-c221eed9` — Mali, lazy descriptors, rebind, packed vertex enabled | FAIL; existing attribute failures remain | Replay PASS; 27 readbacks and all 15 saved images match, including the failed original images. Automatic MMUD activation is used without the loader-check override. |

Both runs retain 12 readbacks whose originals the probe does not save; these
are not counted as matched images. The Turnip fixture exercises padded
non-indexed draws, compact indexed draws, and indexed draws with a shared
offset. Zero stride, overlapping records and an ignored offset at a guard-page
boundary have not been separately exercised on a device. No warning gate was
relaxed. The remaining compile-required warning, arbitrary application replay,
and G04/G06/G10/G12 acceptance remain open.

### Replay with captured pipeline compile-control flags

The compile-control option is [fork commit 1f918617](https://github.com/taowen/gfxreconstruct/commit/1f918617ec0d34c0ee7a23a9b7199bfd5e343283),
which includes the multi-draw and empty-submit repairs. Add
`--replay-preserve-compile-flags` to the desktop runner's `--replay-capture`
command to pass `--preserve-pipeline-compile-flags` to the replay tool.
The explicit setting and full command are saved in `replay-result.json`.

By default, GFXReconstruct removes
`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` and warns. That
warning alone does not establish that the driver needed compilation. The new
option forwards this captured flag through both the ordinary and omitted-cache
pipeline paths. It does not filter diagnostics. Vulkan specifies that a
[compile-required creation produces a null pipeline](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreateFlagBits.html).
If preserving the flag produces `VK_PIPELINE_COMPILE_REQUIRED` where the
capture returned a different result, replay stops at that mismatch before
using the missing pipeline. This mode can therefore reject captures that the
default portable replay could render.

The AArch64 tools were rebuilt from the committed fork source. Fresh expanded
probe evidence, with explicit lazy descriptors and the preserve option:

| Run | Original rendering | Replay |
| --- | --- | --- |
| `20260908T185229-cad9675b` — Turnip, no memory translation | PASS | PASS, no diagnostics; all 38 saved images match across 50 readbacks |
| `20260908T185229-da38cfbb` — Mali, rebind, packed vertex enabled, automatic MMUD | FAIL; existing attribute errors remain | PASS, no diagnostics; all 15 saved images match across 27 readbacks |

Each run still has 12 original packed images not saved by the probe. No new
comparison coverage is claimed for those readbacks. These results replace the
previous Turnip warning failure only for the explicit preserve configuration;
the default mode continues to warn and fail the runner's warning gate.

A separate same-capture comparison restaged Turnip's saved runtime and capture,
then used the tool's built-in `--capture` mode to record actual replay calls.
Both default and preserve modes made 22 graphics/compute pipeline creation
calls with `VK_SUCCESS`. All nine original `0x00000500` graphics flags became
`0x00000400` in default mode and remained `0x00000500` in preserve mode. The
other pipeline flags and results matched the original. Default mode retained
the removal warning; preserve mode had no warning. These recapture checks did
not request resource dumps; the image comparisons come from the fresh probe
runs above. Commands, logs, recaptured traces, converted calls, flag summaries
and file hashes are under that run's `capture/compile-flag-comparison/`.

The same comparison with `MESA_SHADER_CACHE_DISABLE=true` also completed with
the same flag/result observations, retained under `capture/compile-flag-no-cache/`.
It did not trigger `VK_PIPELINE_COMPILE_REQUIRED`. The early-exit branch,
omitted-cache flag preservation, flagged compute/ray-tracing pipelines and
asynchronous creation have code/build coverage only, not separate device
acceptance. Python compilation, shell syntax and rejection of the preserve
option without `--replay-capture` were checked. G04/G06/G10/G12 and arbitrary
application replay remain open.

### Complete packed vertex image retention

The fixed probe now saves every packed vertex case as
`packed-<signed>-<normalized>-<bgra>-<divisor>.rgba`, including failing images.
Each `PACKED_DRAW` log record names its image. A failed write/close makes the
probe fail. The runner collects all twelve files and checks their full pixels
when validating a successful render. Replay checks the filename against the
draw parameters, requires a complete 1024-byte reference, and compares it to
the associated captured copy/submission. Historical logs without image names
retain their explicit uncompared status.

The changed C probe was rebuilt with the existing pinned AArch64 builder,
`-O2 -Wall -Wextra`, without compiler diagnostics. The existing Mesa runtime
hashes were verified before rebuilding; its binaries were retained. The exact
compile command, prior probe/manifest and new manifest are saved under
`build/packed-image-build/`. The new probe SHA-256 is
`b139124c03bb5e2c8e6077bad15120b52f81d65901e3e64d02487949af53ca92`.
Each new run records that probe and its source hashes.

| Run | Original rendering | Replay with lazy descriptors and preserved compile flags |
| --- | --- | --- |
| `20260908T185957-6cf94932` — Turnip, no memory translation | PASS | PASS; all 50 images match; no uncompared images or diagnostics |
| `20260908T185957-32da3cee` — Mali, rebind, packed vertex enabled, automatic MMUD | FAIL; existing attribute errors remain | PASS; all 27 images match, including failed original images; no uncompared images or diagnostics |

Both runs retain all twelve packed images. On a temporary copy of the real
Turnip evidence, removing a packed reference and assigning a different case's
filename were rejected; changing one reference pixel produced exactly one
packed-image mismatch. Results are retained in `capture/packed-image-integrity.json`.
The original evidence was unchanged. Python compilation and diff checks also
passed. This closes the fixed probe's unsaved packed-image gap, not arbitrary
application draw/attachment lineage or G04/G06/G10/G12 acceptance.

### Pipeline-library vertex input provenance

`pipeline_vertex.py` resolves the vertex-input subset separately from draw-state
tracking. It follows captured linked libraries when each pipeline is created,
records library handles/create indices and the vertex-input provider, and saves
the resolved state before libraries can be destroyed. It follows the Vulkan
[graphics pipeline subset rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineLibraryCreateInfoEXT.html),
including the implicit empty subset when linking without an explicit subset.
An absent dependency or multiple vertex-input providers keeps the draw
incomplete. Dynamic attributes/strides are then read from recorded command
state using the provider's dynamic-state declarations.

Previously all 50 Turnip draws were marked incomplete because the final linked
pipeline had no local vertex-input state. Reanalysis of
`20260908T185957-6cf94932` resolves all 50; the corresponding Mali capture
retains its 27 complete records. Reanalysis files are retained separately as
`vertex-libraries-reanalysis.json` without replacing the original summaries.

The final Python modules were byte-compiled and exercised by fresh device
capture/replay with the existing verified C probe/runtime:

| Run | Vertex-state index | Rendering / replay |
| --- | --- | --- |
| `20260908T190556-276762f8` — Turnip | 50/50 complete; first draw's vertex provider is pipeline 43, create call 380; linked library creates are 380/379/381 | Both PASS; 50/50 images match |
| `20260908T190556-bb8da642` — Mali | 27/27 complete; ordinary pipeline state remains directly sourced | Rendering FAIL with existing attribute errors; replay PASS, 27/27 images match |

The run records include the new helper's hash. Temporary mutations of the
preceding real Turnip capture `20260908T190431-46a3f20b` made a linked library
missing or duplicated the vertex provider; both produced incomplete state.
Inserting library destruction after linking preserved the first draw's saved
provider and bindings. Results are retained in `pipeline-library-integrity.json`.
These are evidence-integrity checks, not driver acceptance of invalid pipelines.
Nested library chains and Flags2 handling have code/byte-compilation coverage
only. This index does not reconstruct shader/descriptor state, vertex-buffer
contents, all dynamic-state invalidation rules, GPU indirect arguments or
secondary command buffers. Complete vertex-input metadata is not complete
pipeline execution evidence, and G06 remains open.

### Dynamic vertex stride resource-dump repair

The dynamic-stride repair was introduced in [fork commit e3aa74fb](https://github.com/taowen/gfxreconstruct/commit/e3aa74fb78d651ceb70e45607b020f9ac29ab43f).
The resource dumper recorded `vkCmdBindVertexBuffers2` strides in bound-buffer
metadata but omitted them from the dynamic vertex-input snapshot. With null
`pSizes`, dump sizing therefore used the pipeline's zero strides. The fix
updates the shared dynamic stride state when `pStrides` is provided, preserves
it when strides are not changed, resets the bound size on a bind with null
`pSizes`, and permits zero stride in `vkCmdSetVertexInputEXT`.

This was reproduced on the saved Mali capture
`20260908T190556-bb8da642`, first attribute draw 2103, command-buffer begin 1914,
submit 2128. The original tool reported all strides as zero and dumped only
4/4/4/8 bytes at offset 4. Its after-draw attachment nevertheless matched the
failed `attributes-0.rgba`; that image match did not validate the buffer dumps.
The rejected evidence is retained in `capture/vertex-dump-attempt/`.

The rebuilt tool replayed the **same capture**, with `DumpVertexIndexBuffer`,
`DumpBeforeCommand` and `DumpRawImages` enabled. Results and device-verified
file hashes are in `capture/vertex-dump-verified/`, along with the exact request,
command, replay binary hash, tool manifest and repeatable staging script.

| Binding / captured buffer | Effective stride | Dump offset / size | Compared contents |
| --- | --- | --- | --- |
| 0 / 185 | 16 | 116 / 44 | Three position records starting at vertex 7, including padding |
| 1 / 186 | 8 | 44 / 28 | Four tint records starting at instance 5, including padding |
| 2 / 187 | 8 | 60 / 20 | Three signed integer records, `(-123, 321)`, including padding |
| 3 / 188 | 8 | 60 / 20 | Three half-float records, `(0.5, -2.0)`, including padding |

Every retained byte matches the independently reconstructed source fixture
range. The exported BGRA attachment after the draw, converted to RGBA, still
matches the failed original GL image. This establishes replay GPU buffer
readbacks before one failing draw, not actual hardware attribute fetches or
the cause of the shader's failed checks. The selected draw uses divisor one
and null `pSizes`. General divisor handling, explicit size bounds, indirect
fetch ranges and null-stride/zero-stride edge cases have not been separately
accepted. The fixed tool has no multi-draw resource-dump handler, so equivalent
Turnip multi-draw vertex-buffer evidence is not claimed.

The AArch64 tools were built from the committed fork source. Fresh full image
regression with lazy descriptors and preserved compile flags remains:
Turnip `20260908T191451-aba46d46` rendering/replay PASS, 50/50 images; Mali
`20260908T191452-7de65374` rendering FAIL with the existing attribute errors,
replay PASS, 27/27 images. Both replay logs have no diagnostics or uncompared
images. The new fork pin passes shell syntax and diff checks. G06 and arbitrary
application resource history remain open.


### Instance divisor resource-dump repair

The builder now pins [fork commit ab565b27](https://github.com/taowen/gfxreconstruct/commit/ab565b2792f98c6e0deebf182604f830d6deaed2).
The dumper now retains static pipeline divisor state and dynamic
`vkCmdSetVertexInputEXT` divisors, includes the divisor in JSON binding metadata,
and sizes instance buffer readbacks by the number of distinct instance records.
For a positive divisor this is `ceil(instanceCount / divisor)`; zero divisor
uses one record for a nonempty draw. In accordance with the Vulkan
[vertex-input addressing rules](https://docs.vulkan.org/spec/latest/chapters/fxvertex.html),
`firstInstance` remains the starting record and is not divided.

The previous tool was reproduced on the same saved Mali capture
`20260908T190556-bb8da642`: draw 2220 has four instances, divisor two and
firstInstance five. It omitted divisor metadata and exported 28 bytes at
offset 44, clipping four strides at the allocation end. The corrected export
contains two strides, including padding, for 16 bytes at the same offset.
The before result is retained in `capture/vertex-divisor-before/`.

Three actual device replays of that capture passed with resource dumping,
rebind memory translation and preserved pipeline compile flags:

| Evidence directory under `capture/` | Draw | Divisor / firstInstance | Instance buffer offset / size |
| --- | --- | --- | --- |
| `vertex-divisor-fixed/` | 2220 | 2 / 5 | 44 / 16 |
| `vertex-divisor-zero-first/` | 2335 | 2 / 0 | 4 / 16 |
| `vertex-divisor-one/` | 2103 | 1 / 5 | 44 / 28 |

Each directory retains the request, command, tool manifest, replay binary hash,
device file hashes, reproduction script and verification result. All bytes
of all four bound vertex buffers match the independently reconstructed C
fixture ranges. Binding one reports the expected divisor and stride eight;
the other three ranges remain 116/44, 60/20 and 60/20. Each after-draw BGRA
attachment, converted to RGBA, matches its original `attributes-1/2/0.rgba`.
This verifies the diagnostic readbacks; it does not establish the GPU's actual
attribute fetch or resolve the original attribute rendering failures.

Actual resource-dump coverage here uses static divisor state, dynamic strides,
null `pSizes`, direct draws and divisors one/two. Dynamic divisor updates, zero
divisor, zero instance count and pipeline-library divisor inheritance have
build coverage only. Explicit buffer-size bounds, indirect fetch ranges and
Turnip multi-draw resource dumping remain unaccepted.

The AArch64 fork build and shell/diff checks passed. Fresh full regression:
Turnip `20260908T192330-c4b6ad88` rendering/replay PASS, 50/50 images;
Mali `20260908T192533-f9840bf3` rendering FAIL with the existing attribute
errors, replay PASS, 27/27 images using rebind. Both successful replays have
zero diagnostics, mismatches or uncompared images. An earlier Mali run
`20260908T192449-0cb42e82` accidentally used the default memory mode `none`
and failed at image-view create 414 with `VK_ERROR_OUT_OF_DEVICE_MEMORY`;
that failed evidence is retained and is not counted as replay coverage.
G06 and arbitrary application resource history remain open.


### Validation-log correction for the attribute failures

The independent logger described in [the desktop validation record](README.md#independent-validation-logging-2026-09-08)
now reports four `pNext-09461` violations in the expanded Mali workload.
The divisor-two/nonzero-firstInstance captures above contain invalid Vulkan
draw combinations for this device. Their matching replay buffers/images are
diagnostic readbacks, not acceptance of that Vulkan usage. Divisor one and
zero-firstInstance remain separate controls. Earlier desktop application-log
silence was caused by Zink's empty debug callback, not an established VVL
failure to check the restriction. The rendering compatibility fix remains open.
