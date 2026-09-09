# ICD ownership and allocation probes

Instance, physical-device and device records; allocation refusal and recovery. These tests do not establish lifetime tracking for every Vulkan resource.

## Current shared-layer diagnostics (2026-09-09)

`icd-vk-init` and `icd-life` now use `HYBRIS_VULKAN_TRACE=1` through the same
compatibility layer for both `--backend turnip` and the HAL backend. The
existing checks still require 16 complete instance lifetimes with four live
at once, and 19 complete device lifetimes with live instance parents. Runs
`20260909T194036-fde7cc09` (Turnip) and `20260909T194036-3736031a` (Mali)
pass both workloads and the shared restricted vertex policy check. These
measure layer ownership; adapter WSI generations are separate internal state.

## Historical adapter instance ownership

ICD instance ownership regression: `icd-vk-init` enables the bounded
`HYBRIS_ICD_INSTANCE_TRACE=1` diagnostic and validates 16 unique generations,
matching create/destroy handles and no remaining records. Its evidence parser
hash is recorded in device.json; `icd-vk-init-instances.json` records measured
peak concurrency and reused raw handles. Other workloads keep tracing off.
Rebuilt runs `20260907T033203-27daa0de` (29854870) and
`20260907T033203-b19723b3` (KB2000) each completed **65 PASS / 2 UNSUPPORTED**,
including validation/SyncVal and capture/replay. Both record 16 creations and
16 destructions with zero records remaining. Check each evidence JSON for
observed concurrency/reuse; absence of reuse does not prove reuse handling.
The common 130, Vulkan 643 and ICD 3 defined export sets remain unchanged.
This covers ICD instance records only, not frontend/device/resource state,
custom allocation callback failure/locking, trace truncation or malformed
HAL behavior. Destroy records precede backend destruction and do not imply
GPU completion or driver unloading.


The concurrent instance probe now uses two barriers per round: all four
instances remain alive before the first destroy, and all destroys finish
before any next-round create. All workers complete the barriers even after
create/query/global-enumeration errors; partial thread startup cancels before
entering them. `instance_evidence.py` now requires peak_live=4, in addition to
16 unique, paired lifetimes. The previous peak-2 and peak-1 trace files were
rejected by this stronger gate. This verifies overlapping object lifetimes,
not parallel execution inside the HAL, handle reuse or error-injection paths.
Rebuilt runs `20260907T033524-371d9b4f` (29854870) and
`20260907T033524-e279f2dc` (KB2000) each completed **65 PASS / 2 UNSUPPORTED**,
including native/hybris/ICD concurrent instance workloads and validation/capture.
Both evidence files report created=16, destroyed=16, remaining=0, peak_live=4,
reused_handles=0. Thus actual raw-handle reuse remains unverified.

## Application allocation callbacks

`vk-alloc` supplies application allocation/reallocation/free callbacks for
three instance create/query/destroy cycles and checks that outstanding callback
allocations return to zero after each destruction. A final create refuses
all callback allocations and requires VK_ERROR_OUT_OF_HOST_MEMORY without
outstanding allocations. It runs through native, replacement frontend and
standard ICD. The callback allocator preserves alignment and realloc contents;
this does not test every allocation failure position, callback locking,
device/resource callbacks or concurrent allocation races.
Rebuilt runs `20260907T033851-afd02fc4` (29854870) and
`20260907T033851-da205be7` (KB2000) each completed **68 PASS / 2 UNSUPPORTED**,
including all three allocator paths, validation/SyncVal and capture/replay.
All successful cycles ended with live=0, and refusal returned -1
(VK_ERROR_OUT_OF_HOST_MEMORY). The standard loader may reject allocation
before reaching the ICD; this is API-boundary failure evidence, not proof of
failure at the ICD record allocation or every HAL allocation site.

## Direct ICD allocation refusal

`icd-alloc-direct` explicitly loads the optional adapter and calls its
`vk_icdGetInstanceProcAddr` entry, without loading the standard Vulkan loader.
Its first application allocator call refuses the adapter's instance record;
the probe requires OUT_OF_HOST_MEMORY, exactly one allocation attempt and
zero outstanding allocations. It then restores allocation and completes three
instance create/query/destroy cycles, followed by another allocation refusal.
This is a direct ICD/HAL boundary workload, not standard-loader or layer-chain
coverage. Existing `vk-alloc` cases retain those separate paths. No dispatch
header rewriting is needed for these direct instance queries/destruction.
Rebuilt runs `20260907T034217-545f66e6` (29854870) and
`20260907T034217-f1c385c1` (KB2000) each completed **69 PASS / 2 UNSUPPORTED**,
including validation/SyncVal and capture/replay. Both direct cases record
initial-reject=-1, calls=1, live=0; each recovery cycle also ends at live=0.
Their final mapping records contain neither the standard nor Android Vulkan
loader. This proves the tested adapter allocation refusal and subsequent
recovery, not all allocation failure positions or callback locking interactions.


The direct allocator workload also allows the adapter record allocation and
then refuses all subsequent allocations, exercising HAL creation failure and
adapter-record rollback. It requires at least two allocation attempts,
OUT_OF_HOST_MEMORY and no outstanding callback allocations, then restores
allocation for three normal cycles. This targets the first required HAL
allocation on the tested driver, not every optional allocation or failure site.
Allocation callbacks must not call Vulkan commands; earlier references to
callback reentrancy were not a valid API-conformance requirement. See
[Vulkan host allocation rules](https://docs.vulkan.org/spec/latest/chapters/memory.html).
Callbacks still run outside the instance table guard to avoid holding it
across application allocator locks and work.
Rebuilt runs `20260907T034553-3c4de9ce` (29854870) and
`20260907T034553-5b6f5c0c` (KB2000) each completed **69 PASS / 2 UNSUPPORTED**,
including validation/SyncVal and capture/replay. Both direct cases report
HAL-reject=-1, calls=2, live=0, followed by three successful recovery cycles.

## Device ownership and recovery

ICD device ownership: `icd/device.c` stores per-device generation, parent
instance generation, backend GDPA/destructor and allocation callbacks. Physical
handles from ordinary/core/KHR group enumeration are associated with their
instance. `icd-life` enables bounded instance/device traces; the runner checks
19 paired device lifetimes, at least two live devices, valid live parents and
zero remaining records. Trace retirement precedes backend destruction and is
not GPU-completion evidence. The evidence parser rejects wrong parents,
missing destroys and duplicate creates in mutated copies of an actual log
(`20260907T051844-72e121b6/device-negative-check.log`).

The extended allocator probe rejects device creation, restores allocation,
obtains a queue and destroys the device/instance. Native, replacement frontend
and standard-loader paths use ordinary physical enumeration. Direct ICD alone
uses ordinary, core group and KHR group enumeration in separate instances;
each rejected adapter allocation makes exactly one callback and leaves the
live allocation count unchanged. Each recovered round finishes with no live
allocations. No callback invokes Vulkan while executing.

Full library/probe builds and final runs `20260907T052046-87a800b9` (29854870)
and `20260907T052046-9a4d3d37` (KB2000) each completed **92 PASS /
2 UNSUPPORTED**, including validation, SyncVal and ordinary/dynamic
capture/replay. Both device evidence files report 19 creations/destructions,
remaining=0 and peak_live=2; respectively two and three distinct raw handles
were reused across lifetimes with different generations. ICD still exports
exactly its three loader entrypoints. This is optional ICD metadata evidence,
not resource-generation coverage or replacement-frontend state management.

Exploratory runs `20260907T051844-72e121b6` / `20260907T051844-288595ec`
used KHR group enumeration in all allocator paths. Native and replacement
frontend crashed after that enumeration, whereas direct and standard-loader
ICD passed. The Android loader/driver cause remains unresolved; these failures
are retained and the final non-direct probe deliberately does not claim KHR
group coverage. Physical-inventory allocation failure, multiple physical GPUs,
trace truncation, arbitrary driver allocation failures and malformed HALs
remain unverified. No unit-test suite was added.

## Physical enumeration allocation refusal (2026-09-07)

Allocator workloads are separated into `probe_alloc.c`; `probe_vulkan_init.c`
retains the concurrent first-entry workload. The existing `icd-alloc-direct`
case now refuses allocation callbacks during the first physical-handle
enumeration, after a count-only query. It retries on the same instance with
allocation restored, then exercises device allocation refusal and recovery.
Fresh instances cover ordinary enumeration, core device-group enumeration
and the enabled KHR group alias. Each round destroys the instance and requires
zero outstanding callback allocations. Unsupported KHR group enumeration
retains the existing fallback to the ordinary route; these two devices ran
all three distinct routes.

| Device | Result directory | Selected cases |
|---|---|---|
| 29854870 / Adreno 650 | `20260907T152413-b3684874` | 9 PASS |
| 10AFA31610002QH / Mali-G1-Ultra | `20260907T152414-8e85ae84` | 9 PASS |

Both builds use the existing pinned builder and NDK 29.0.13846066. Cases are
native/hybris/ICD `vk-alloc` and `vk-init`, hybris `command-alloc`, direct ICD
allocation, plus the runner's ICD version check. Both runners verify staged
provenance and required mappings. Mali uses the existing explicit loader
quirk. All six physical rejection events report OUT_OF_HOST_MEMORY, one
callback attempt and zero live-count delta. The subsequent device recovery
succeeds, and each of the six instance rounds ends with zero live allocations.
The reported failure-path output count is observational, not a portable
success condition.

This callback refusal spans adapter and HAL calls; it does not isolate every
HAL allocation site. It does not cover partial enumeration of multiple GPUs,
concurrent enumeration, allocator-internal locking or resource ownership
beyond these instance/physical/device paths. Validation/SyncVal and GPU
rendering were not rerun for this probe-only change. G03 remains open.
