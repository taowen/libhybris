# Implementation status

The active objective is to complete the acceptance criteria in [gaps.md](gaps.md)
and improve maintainability without changing API/ABI contracts during structural
refactors. A probe passing does not close an entire gap.

## Delivery order

1. Split existing code along ownership boundaries; preserve exported ABI and
   run the existing device baseline after each production change.
2. Finish independent build/provenance (G01), then generated Vulkan entry
   coverage and per-object dispatch (G02/G03).
3. Prove standard loader/layer integration (G04) before compatibility
   transformations; retain ordinary passthrough as a comparison.
4. Add capability differences and bounded draw/resource evidence (G05/G06/G12).
5. Implement only format/shader/synchronization behavior with a
   reproducible workload, precise capability gating and reference output.
6. Evaluate desktop GL requirements, implement WSI lifecycle coverage, and
   reproduce application failures on their original devices.

Independent smoke/semantic/device workloads are used for verification. No new
unit-test suite or general capture/replay engine is planned. Completed batches
are committed and pushed separately to taowen/ardesk.

## Remaining acceptance work

| Gap | State | Next concrete evidence |
|---|---|---|
| G01 | AArch64 baseline verified | Standalone default build, fixed inputs/toolchain, library/probe hashes and sampled runtime mappings verified on two devices; see baseline README. |
| G02 | Partial | 726-command pinned registry query table and four-route transfer workload verified; core/KHR memory2 calls verified. Still need per-object dispatch/compat state and broader enabled-feature semantics. |
| G03 | Partial | Observe TLS allocation/destruction and generation handling; exercise GLES multiple contexts and cross-thread teardown. |
| G04 | Partial | Standard-loader → vendor-HAL ICD headless path passes eight cases on both devices. Standard glibc VVL legal/illegal lifecycle cases also pass on both devices. Fixed headless widget capture/replay now matches all RGBA bytes; still need a presented frame and application/WSI coverage. |
| G05 | Partial | Machine-readable raw/effective features2, limits/extensions/format queries and corresponding CreateDevice behavior. |
| G06 | Partial | Associate an injected wrong binding with the first wrong draw and effective descriptor/resource generation. |
| G07 | Open | Reference pixels for each supported format and upload/copy/view/subresource path; reject unsupported semantics. |
| G08 | Open | Fixed SPIR-V tooling, reflection/hash/specialization records, before/after semantic evidence for each transform. |
| G09 | Open | Noncoherent/staging/reuse and queue ordering cases; validate emulation against completion and memory visibility. |
| G10 | Open | Fixed Mesa/Zink revision and GL target-profile requirements; GL workload results with backend attribution. |
| G11 | Open | Xlib/XCB/Wayland surfaces and resize/release/fence/FD lifecycle through the matching receiver. |
| G12 | Open | Opt-in bounded evidence package associating shader/resource/draw/image/submit/present. |
| G13 | Open | Versioned multi-vendor baselines, selected fixed CTS cases and original Blender failure fixtures. |

## Structural work

- common/linker_bridge.c owns Android linker selection, initialization,
  backend entry pointers and public android_*/hybris_* loader entry points.
- common/hooks.c retains libc hooks, hook selection and current TLS storage.
  linker_bridge.h is private; new cross-file helpers have hidden visibility.
  Existing exported backend pointers and TLS callbacks retain their ABI.
- Baseline probes are independent translation units by responsibility; shared
  declarations/check helpers are in probe.h and probe_common.c. No probe
  semantics or case list change is intended by this split.

Remaining large-file work includes pthread/libc/TLS separation inside common,
further WSI/backend state separation, and targeted review of
large platform/driver files. Imported Android linker sources should retain
their upstream structure unless a concrete fix requires changing them.

## Device coverage

Currently connected: 29854870 (M2012K11AC) and 192.168.1.28:5555 (KB2000),
both Android SDK 33. Both now have standalone smoke results in the baseline README.
Neither is the original Mali-G1-Ultra or Adreno 830 Blender failure device.
Work independent of those devices continues; their absence does not justify
claiming the application regressions are fixed.

## Latest verified batch

Structural split: fresh AArch64 library/probe builds; all 130 defined dynamic
exports of libhybris-common match the pre-split ABI (name, type, binding and
visibility). Device run 20260906T233801-473a9d70: 21 PASS, 2 UNSUPPORTED
(desktop GL). This preserves the existing baseline, not broader conformance.


Standalone build/provenance: repository-owned digest/snapshot-pinned toolchain,
fixed downloaded headers with cached-content verification, source/header/probe
snapshots and manifests, actual target pkg-config versions, runtime maps and
Android mapped-file hashes. Independent checkout builds and two device runs
completed (21 PASS / 2 UNSUPPORTED each). Modified cache and executable
checks failed as intended. Next priority is G02/G04 Vulkan loader integration.


Standard-loader batch: optional vendor-HAL ICD, interface version 5, generated
physical-device resolver scope from pinned Vulkan-Headers v1.4.309. Existing
frontend remains default. Two device runs each completed 29 PASS / 2 UNSUPPORTED
including eight ICD cases. No dispatch header rewriting or creation-chain
stripping. Next: standard glibc validation layer and capture tooling, plus
version/extension-dependent dispatch coverage.


Standard validation batch: pinned glibc VVL package with its original JSON,
explicit layer/debug-utils activation and a callback-checked negative case.
Two devices each completed 30 PASS / 2 UNSUPPORTED; legal lifecycle zero
errors and injected zero-size buffer exactly one expected VUID, aborted
before vendor execution. Capture/replay and validation of the full rendering
workload remain open.


Widget validation: the standard layer now observes both full widget fixtures
with SyncVal enabled. Each device passed the original exact pixels with zero
ERRORs (31 PASS / 2 UNSUPPORTED overall). This closes the missing validation
coverage of this fixture, not G04's remaining capture/replay requirement.


Headless capture/replay: fixed GFXReconstruct c2ff0eecc7a7f43aa236a5c98097a685b928b782,
AArch64 tools and runtime dependencies built in the repository snapshot-pinned
container. The runner captures each widget binding separately and associates
the replay resource with its copy/submit indices. All 1024 RGBA bytes must match
the uncaptured probe, captured probe and replay. Tool hashes/source/container
identity and capture/API JSON/resource reports remain in the evidence package.
This does not contain a present frame or establish application/WSI replay.
Verified on 29854870 (`20260907T004458-8576f807`) and KB2000
(`20260907T004459-930906e9`): 32 PASS / 2 UNSUPPORTED each, including VVL
and SyncVal. The first capture attempt exposed a missing indirect xxhash
runtime dependency, now included by the builder.


Registry/entrypoint batch: generated scope, alias and provider metadata for 726
Vulkan commands from the same fixed registry as the ICD resolver. Runtime
queries record per-name dlsym/GIPA(NULL)/GIPA(instance)/GDPA availability and
check 137 core 1.0 entries plus forbidden non-global/non-device scopes. All
core 1.0 addresses are also referenced by the linked binary. The existing
fill/fence/readback workload now actually executes through link, dlsym, GIPA
and GDPA; earlier linked-vk results only proved dependency loading because
its calls still used GIPA. Linked dispatch already exercised create/destroy.
Memory2 core 1.1 and enabled KHR variants match ordinary buffer requirements
and complete the same readback. On 29854870 (`20260907T005630-74134b03`) and
KB2000 (`20260907T005631-3e16a681`), each run is 45 PASS / 2 UNSUPPORTED,
including unchanged VVL/SyncVal and full-image capture/replay. Pointer presence
is not command-execution coverage; per-object compatibility state and the
unsupported modern aliases remain open.


Vulkan export split: `vulkan_exports.c` owns the unchanged AArch64 trampolines,
header/platform-guarded export list, missing-symbol diagnostic and bulk pointer
resolution. `vulkan.c` retains Android-library ownership, proc-query and WSI
wrappers; its existing constructor calls the hidden export initializer after
loading the backend. The frontend file decreases from 1036 to 257 lines.
A fresh AArch64 build preserves all 643 defined dynamic exports by name, type,
binding and visibility. No extra public helper is exported. Runs on 29854870
(`20260907T010200-b47ca796`) and KB2000 (`20260907T010201-038aaa84`) each complete
45 PASS / 2 UNSUPPORTED, and all five 726-command query reports exactly match
the pre-split runs. VVL/SyncVal and full-image headless capture/replay also pass.
This structural change does not add per-object state or new API support.


Capability observations: registry-generated core feature/limit/sparse fields,
device extension versions and ten format/image-format queries now produce
named JSON values and native-to-frontend/ICD differences. Runs
`20260907T010645-554aab1d` (29854870) and `20260907T010646-7003fc92` (KB2000)
each complete 42 PASS / 2 UNSUPPORTED; validation/capture options were not
selected for this probe-only batch. Each native observation contains 325
values. The frontend matches native; direct-HAL ICD differs in six device
extensions related to Android buffer/presentation. No values are rewritten
to force equality. Features2 extension chains and format execution remain open.


Features2 batch: a separate probe verifies five core 1.1 feature structures,
55 named core fields against the legacy query, intact pNext pointers, positive
CreateDevice using the returned chain, and exact FEATURE_NOT_PRESENT for a
false float64 bit enabled through the same chain. Both devices complete
45 PASS / 2 UNSUPPORTED in runs `20260907T011042-0affc534` and
`20260907T011043-11791534` (optional VVL/capture not selected). Native/frontend/ICD
66-value feature records match. This does not close all extension chains,
properties2 or actual shader execution; G05 remains partial.
