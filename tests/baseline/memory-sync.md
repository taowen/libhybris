# Vulkan memory and synchronization probes

Timeline host operations, queue visibility, non-coherent ranges and partial mappings. Each workload reports required capabilities separately; unsupported memory or queue types are not counted as passed execution.

## Timeline host commands

Timeline semaphore dispatch (2026-09-07): standalone `timeline-core` requests
API 1.2; `timeline-khr` requests API 1.1 and enables KHR_timeline_semaphore.
Both query/enable the timelineSemaphore feature. `-gdpa`, `-elf` and `-linked`
select separate command paths; the unsuffixed case uses GIPA. An initial host
wait is released by a signal on a second thread, followed by four queue
wait-before-host-signal submissions. Each starts with a zero-timeout host wait,
submits a future timeline wait/signal pair, verifies the fence is not yet ready,
then host-signals the dependency, waits on the timeline and fence, checks the
exact counter (3, 5, 7, 9), and resets/reuses the fence. No queue/device wait-idle
is used. The semaphore protocol follows the
[Khronos timeline sample](https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html).
These submissions contain no command buffers and do not prove resource memory
visibility or multiqueue execution. The delayed signal thread exercises ordinary
cross-thread host operations, not exhaustive scheduling or thread-safety proof.

Old frontend `20260907T073632-06569d89` passes KHR GIPA but aborts through
KHR ELF/link at GetSemaphoreCounterValueKHR. Native KHR GIPA passes as well.
The frontend now resolves all three host timeline commands by the supplied
registered device and exact core/KHR name; no Android core-export fallback or
feature emulation is involved. The 643 public Vulkan export names are unchanged.

Final full X300 `20260907T074051-325fe194`: **135 PASS / 10 UNSUPPORTED /
1 CRASH**. Final Redmi `20260907T074052-c6dc686b`: **98 PASS / 47 UNSUPPORTED /
1 CRASH**. The remaining crash on each is native-groups. X300's ICD path uses
the explicit scoped Mali option. All frontend timeline routes pass on X300;
standard-loader core/KHR validation variants have errors=0 with synchronization
validation enabled. Native/standard-loader absent KHR ELF exports remain
UNSUPPORTED. Redmi does not provide the required API/extension and all 24 new
cases remain UNSUPPORTED. Existing validation and both capture/replay gates pass.
No unit-test suite was added. Timeline import/export, multiple semaphores with
WAIT_ANY, concurrent monotonic signal ordering, multiqueue dependencies,
resource visibility, and destruction with outstanding work remain uncovered.

## Cross-queue timeline data visibility (2026-09-07)

`timeline-queues-core` requests API 1.2; `timeline-queues-khr` requests API 1.1
and enables KHR_timeline_semaphore. Both require the timeline feature and two
graphics/compute queues in the same family. The additional
`-validation` modes use the standard ICD with Khronos validation and SyncVal.
They share instance/device setup with the existing single-queue workload;
GPU transfer and readback live in `probe_timeline_queues.c`.

Each of four cycles first submits a consumer that waits for a future timeline
value, copies 4096 bytes, makes the copy visible to host reads and signals the
next value. Its fence must still time out before the producer is submitted.
The producer then fills the source buffer with a cycle-specific word and
signals the consumer's dependency. On later cycles it also waits for the
previous consumer's signal, ordering resource reuse on the GPU. The host waits
for the consumer value and both fences, invalidates the mapped readback
allocation, and checks all 1024 words before resetting fences and re-recording
both command buffers. No queue/device wait-idle is used. Mappings are captured
while the device and its resources still exist.

The ordering follows the Khronos [timeline semaphore
sample](https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html)
and [semaphore memory dependencies](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-semaphores).
The probe prefers a genuinely non-coherent host-visible readback memory type
when available and logs the selected flags. Invalidation covers the whole
mapped allocation from offset zero; it does not test partial-atom ranges or
host-written uploads/flushes.

| Device/backend | Evidence | Results |
|---|---|---|
| X300 / Mali vendor, frontend and standard ICD | `20260907T153355-ea6e0794` | 9 PASS: six dual-queue paths, two existing single-queue validation controls and ICD version |
| Redmi 29854870 / Adreno vendor, frontend and standard ICD | `20260907T153355-3c3c999e` | ICD version PASS; eight timeline cases UNSUPPORTED |
| Redmi / independent official Turnip `c3b008c1` control (no libhybris) | Parent `build/mesa-upstream/results/turnip-f2040a95` | Both dual-queue core/KHR validation modes UNSUPPORTED: family 0 exposes one queue |

Mali exposes two queues in family 0 (flags `0x17`); the workload verifies that
their handles differ. All six dual-queue paths use memory flags `0xb`, which
are host-visible and non-coherent. Each checks 4096 words over four cycles
with zero mismatches and counter values 2/4/6/8. The two dual-queue ICD runs
and two single-queue controls report `validation=1 errors=0`. The runner
verifies staged provenance and required mappings. Mali retains the existing
explicit scoped loader quirk; no driver capability is overridden.

The Turnip row is a separate backend control using the same standalone Vulkan
probe through the standard loader directly to Turnip. It stages no libhybris
libraries and does not pass through the hybris ICD. The libhybris paths above
continue to bridge the Android vendor drivers on both Mali and Qualcomm.
Qualcomm can use either its vendor driver through libhybris or the independent
Turnip backend. This Adreno vendor driver's missing timeline capability is
not a general libhybris/Qualcomm incompatibility. Turnip was staged from the
unmodified official build, with driver bytes
checked against its build output and the current Mesa commit stamp. Its
result retains commands, probe manifest and staged hashes. It returns before
GPU work; the final mapping snapshot after instance destruction does not
retain Turnip, so this record is not a live-driver mapping proof or a rendering
pass. Adreno vendor core/KHR requirements fail before queue work as well.

This adds same-family queue ordering, deferred signal submission, repeated
buffer reuse and full-allocation non-coherent invalidation coverage. It does
not prove physical GPU overlap, different-family ownership transfers,
concurrent host submissions, partial flush/invalidate ranges or legal resource
retirement while other work remains in flight. G09 remains open.

## Partial non-coherent upload and readback (2026-09-07)

`memory-ranges` is a Vulkan 1.0 workload for native, frontend and standard ICD;
`memory-ranges-validation` adds standard validation/SyncVal. The standalone
implementation is `probe_memory_ranges.c`. It requires compatible host-visible
non-coherent memory for both upload and readback buffers, rather than silently
substituting coherent memory. Both buffers have a nonzero memory binding
offset aligned to the buffer requirement and nonCoherentAtomSize.

The upload buffer is initialized once. Each of the first four rounds changes a smaller
region that starts inside an atom and crosses atom boundaries, flushes the
atom-rounded range including the binding offset, and resubmits the same
command buffer. A GPU copy moves a larger window, including unchanged prefix
and suffix bytes, to a different destination buffer offset. Transfer-to-host
barriers and a fence precede partial-range invalidation and an exact comparison
of the entire copied window. Host writes begin only after the previous round
has finished; no queue/device wait-idle is used. The probe captures mappings
before destroying the live objects.

Range arithmetic follows [VkMappedMemoryRange](https://docs.vulkan.org/refpages/latest/refpages/source/VkMappedMemoryRange.html):
flush/invalidate offsets are relative to the memory allocation and atom
aligned. The original four rounds use finite atom-multiple sizes inside the mapping.
This is valid-input coverage; it does not inject unflushed writes or assert
that a particular driver must expose stale cache data for invalid input.

| Device | Result directory | Results |
|---|---|---|
| X300 / Mali-G1-Ultra | `20260907T154415-7f579232` | 5 PASS: four memory-range paths plus ICD version |
| Redmi 29854870 / Adreno 650 vendor | `20260907T154415-b91d1678` | ICD version PASS; four memory-range paths UNSUPPORTED |

Mali selects memory type 1, flags `0xb`, for both buffers. The atom size is
64 bytes, each buffer binds at offset 64 within a 2112-byte allocation, and
all four rounds flush `[576,896)` and invalidate `[576,1600)`. Each compares
1024 bytes, including unchanged neighbors, with zero mismatches across all
four paths. The standard ICD validation run reports zero errors, including
SyncVal. Adreno vendor exposes no compatible host-visible non-coherent memory
for this workload; native, frontend and ICD agree. Mali uses the existing
explicit loader quirk. The AArch64 glibc/bionic probe build, staged provenance
and required runtime mapping checks pass.

The 2026-09-08 extension preserves those four rounds and adds three. Round 4
unmaps and remaps only the buffer at a nonzero allocation offset, using
VK_WHOLE_SIZE flush/invalidate that end at the mapping boundary before the
allocation ends. Rounds 5 and 6 map only the final half of each buffer plus
allocation padding, and copy through the last byte of the buffers. Round 5
uses finite ranges ending at the allocation boundary with non-atom-multiple
sizes; round 6 uses VK_WHOLE_SIZE to that same boundary. CPU access subtracts
the current mapping origin, while flush/invalidate retain allocation offsets.
The command buffer is re-recorded once for the final copy window and reused in
the last round. Every round still checks all 1024 copied bytes, including
unchanged neighbors, after a fence; no wait-idle was added.

| Device | Result directory | Results |
|---|---|---|
| Mali X300 | `20260908T152835-5afde30e` | Four paths pass all seven rounds; standard validation/SyncVal reports zero errors |
| Redmi Adreno 650 vendor | `20260908T152835-aed95da5` | Four paths UNSUPPORTED: no compatible non-coherent memory type |

The updated NDK bionic/glibc probe build completed. On Mali the atom is 64 and
the allocation is 2144 bytes, ending 32 bytes into an atom. Round 4 maps
`[64,2112)`; its WHOLE_SIZE ranges start at 576 and end at 2112, not 2144.
Rounds 5/6 map `[1088,2144)`. Round 5 flushes `[1344,2144)` (800 bytes) and
invalidates `[1088,2144)` (1056 bytes); round 6 uses the same starts with
WHOLE_SIZE. The logs retain mapping extents, allocation sizes, raw ranges,
readback counts and results for native, frontend and ICD independently.

This covers partial mappings and allocation-end partial atoms on this Mali
driver, in addition to the earlier ranges and resubmissions. Simultaneous access
to different atoms, cross-process mappings and unrelated work remaining in
flight during resource retirement are still untested. G09 remains open.
