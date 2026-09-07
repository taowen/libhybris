# Window validation/capture review, 2026-09-07

The user's changes are preserved in `57cb045`. This review corrects the test
verdict and evidence handling; it does not change the production ICD or vendor
libraries. Validation and capture are separate workloads with the pinned tools.

## Corrections

- Count ERROR callbacks atomically: the surface lifecycle workload makes Vulkan
  calls from four threads. Print and check the total **after** `vkDestroyInstance`,
  then finish Wayland cleanup even on validation failure. The create-info pNext
  callback remains responsible for instance creation/destruction messages.
  Runs without validation no longer print a misleading `errors=0` record.
- The host also rejects any ERROR callback line anywhere in the complete log,
  including one after an older probe's prematurely printed zero total.
- Retain `capture/window.gfxr` before conversion/replay, or after a failed client,
  and compare its host/device SHA-256. Keep the six compared replay binaries,
  conversion/replay command lines, exit codes, elapsed times and timeout logs.
  Capture helper hashes and the original validation manifest are recorded.
- Launch each tool with a separate `capture-tool.pid`. On timeout, kill only that
  PID after confirming its working directory belongs to this run. A conversion,
  report-read or replay exception cannot silently bypass capture failure handling.
  Tool timeouts produce TIMEOUT; nonzero tool exits produce FAIL with their actual
  exit code retained in `capture/commands.json`.
- Reject inconsistent duplicate transfer indexes, unsupported comparison formats,
  truncated dump data and incorrectly ordered begin/copy/submit/present sequences.
  Keep support for the pinned tool's null-padded transfer arrays.
- Preserve an explicit `--timeout 65`; only an omitted timeout selects the layered
  default of 180 seconds. The isolated wrapper forwards `--probe` so independently
  built negative-control probes can use the same compositor isolation.

Callback contracts:
[Khronos debugging](https://docs.vulkan.org/spec/latest/chapters/debugging.html)
permits simultaneous callbacks from different calling threads;
[instance initialization](https://docs.vulkan.org/spec/latest/chapters/initialization.html)
defines the create-info debug callback's instance-destruction coverage.

## Supported runs

The normal probe was rebuilt with the pinned AArch64 builder; its current C
sources match the build snapshot. Production libraries were unchanged and their
existing bundle manifest was verified during staging. Both phones use the
existing dedicated `io.taowen.hybriswsitest` compositor. The Qualcomm device is
OnePlus 8T `192.168.1.28:5555`; Mali is `10AFA31610002QH`. No Redmi run was used.

| Workload | Device | Isolated run / client run | Result |
| --- | --- | --- | --- |
| VVL + SyncVal + swapchain boundaries | OnePlus 8T | `20260907T231116-72e95e43` / `20260907T231118-7e2ecb43` | PASS, final errors=0 |
| VVL + SyncVal + swapchain boundaries | Mali | `20260907T231116-bb226927` / `20260907T231117-2c8af751` | PASS, final errors=0 |
| Capture + virtual replay | OnePlus 8T | `20260907T231357-66310c2c` / `20260907T231358-a1bdd7c2` | PASS, 24 copy records, six exact comparisons |
| Capture + virtual replay | Mali | `20260907T231357-012a6f00` / `20260907T231357-c9a9b801` | PASS, 24 copy records, six exact comparisons |

All four include the 128-surface/16-concurrent lifecycle workload and the normal
24-frame, three-size window with six screenshot/readback comparisons. The two
validation runs also include the timeout, retirement, allocator, multi-present
and socket-release boundary checks. Frame mappings contain the requested layer,
standard loader and ICD, without Android libvulkan. Compositor identities stayed
stable; the wrapper reported no remaining compositor PID after cleanup.

The retained capture inputs and six replay binaries were independently rehashed
and compared again on the host. Missing/extra/conflicting transfer records were
rejected using mutations of the actual replay report. These checks are recorded
under `tests/wsi/build/capture-review-fault/evidence-audit.json`.

## Negative controls

A separate build of both `57cb045`'s probe and the corrected probe omits only the
final `vkDestroySurfaceKHR`. The ICD's existing orphan cleanup permits the process
to finish, but VVL must report `VUID-vkDestroyInstance-instance-00629`.

| Probe | Device | Isolated run / client run | Observed verdict |
| --- | --- | --- | --- |
| Original + omitted destroy | OnePlus 8T | `20260907T231513-47f760ab` / `20260907T231515-da4f0dfd` | Incorrect PASS, ERROR occurs after errors=0 |
| Original + omitted destroy | Mali | `20260907T231513-279a95f0` / `20260907T231514-0e3d24e5` | Incorrect PASS, ERROR occurs after errors=0 |
| Corrected + omitted destroy | OnePlus 8T | `20260907T231553-81e3a5ba` / `20260907T231555-3a48a00b` | Expected FAIL 2, errors=1 |
| Corrected + omitted destroy | Mali | `20260907T231533-84184c97` / `20260907T231534-a9a6cd8e` | Expected FAIL 2, errors=1 |

The original controls used the old summary-only host verdict. The final host
log gate additionally rejects those retained original logs. Fault probe source,
ELFs and source/hash manifests remain under `tests/wsi/build/capture-review-fault/`;
they are not the normal probe build. No negative-control error is counted as a
normal rendering PASS.

A standalone tool-runner audit on both phones ran an owned Android `sleep` process
through the capture-tool launch path with a one-second timeout, then a tool that
exits 7. Partial logs, timeout/exit records and PID/cwd-checked removal all passed.
The script and per-device reports are in the same ignored fault-artifact directory.
This is a process-management audit, not simulated Vulkan execution or a unit-test
framework.

## Rejected combinations and scope

Two previously accepted option combinations failed on both phones and are now
rejected before launching or stopping a compositor:

- `--validation-layer` with `--capture-tools`: pinned GFXReconstruct 1.0.5 adds
  `VK_KHR_depth_stencil_resolve` without enabling its `VK_KHR_create_renderpass2`
  dependency. VVL reports `VUID-vkCreateDevice-ppEnabledExtensionNames-01387`.
  Combined failures are OnePlus `20260907T230941-8a312973` and Mali
  `20260907T230941-f6bc1afd`. Their raw captures remain saved. The pinned tool's
  `framework/graphics/vulkan_device_util.h` extension set confirms the omission;
  no tool-source or ICD capability change was made to conceal this error.
- `--swapchain-review` with `--capture-tools`: the boundary workload deliberately
  fails one application allocation to retire the old swapchain. Replay does not
  reproduce that callback failure; its later successful replacement instead
  returns `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`, then the tool exits 139. Failures
  are OnePlus `20260907T231211-2cf72461` and Mali `20260907T231211-41431879`.
  The `.gfxr`, converted calls and exact tool error/exit logs remain saved.

Separate VVL and capture PASS results do not establish that the capture layer's
injected calls are validation-clean. Virtual replay checks the probe's copied
pixels; it does not present a second window, establish arbitrary application
replay, implement swapchain image aliases, or close the full G04 gate.
