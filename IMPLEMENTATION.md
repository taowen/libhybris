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
| G02 | Partial | Pinned vk.xml coverage for direct/link/GIPA/GDPA entries, alias/enablement rules and per-device dispatch. |
| G03 | Partial | Observe TLS allocation/destruction and generation handling; exercise GLES multiple contexts and cross-thread teardown. |
| G04 | Partial | Standard-loader → vendor-HAL ICD headless path passes eight cases on both devices. Still need legal/illegal validation and frame capture/replay with matching pixels. |
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
Vulkan generated exports versus frontend/WSI logic, and targeted review of
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
