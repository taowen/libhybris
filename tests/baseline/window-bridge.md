# Window bridge regression history

Historical wrapper-scope, fallback and runtime-dependency records. Current window behavior and acceptance belong to [the WSI suite](../wsi/README.md), not the headless smoke test. Statements below describe the dated revisions.

## WSI wrapper resolution scope

The swapchain wrapper now uses the current device's GDPA to obtain its backend
function before doing WSI preparation. Wayland create/destroy obtains backend
functions from the supplied instance's GIPA on each call; no first-instance
function pointer is retained globally. Missing creation functions are rejected
before allocating Wayland objects. A missing destroy function for a mapped
surface produces an explicit fatal diagnostic before removing the mapping.

`wsi-disabled` is a hybris-only negative case built on the capability workload.
It creates a device with no extensions, requires GDPA(CreateSwapchainKHR) to be
NULL, then deliberately calls the direct export with a zeroed swapchain create
description. The frontend must reject this with EXTENSION_NOT_PRESENT before
using WSI. This is an intentional invalid application call testing a hybris
guard policy, not a legal Vulkan workload or a normative Vulkan error test.
It is not run against native/ICD or validation paths.

Before the fix, `20260907T022910-9f4bfd8e` on 29854870 returned success (0)
from the direct export despite GDPA returning NULL. No usable swapchain is
established by that result. The new path returns -7 instead of using a global
ELF entry that can bypass device enablement.

Wayland changes are build-checked only: no compositor/surface lifecycle is
exercised by this headless negative case. Full per-instance/device state,
surface-capability wrapper resolution, mapping concurrency, destruction order,
generation tracking and successful swapchain creation/presentation remain open.

Fresh library/probe builds and runs `20260907T023048-a47b0dd0` (29854870) and
`20260907T023048-bac54a9c` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. The negative case returns -7 on both
devices. Vulkan's 643 dynamic exports remain unchanged.

## Surface capability fallback policy

The surface-capability wrappers now resolve Android-loader ELF trampolines
during frontend construction, eliminating lazy pointer writes from concurrent
calls. Missing entries return EXTENSION_NOT_PRESENT. Capabilities2 no longer
falls back to the legacy query, which cannot process the caller's input/output
pNext chains, or tries GIPA(NULL) for a physical-device command.

The 643 Vulkan dynamic exports remain unchanged. This is a code-review fix
with build and headless regression coverage, not a reproduced surface query
failure: no live WSI surface or missing-entry injection is exercised. Resolution
still uses Android-loader ELF trampolines; physical-device-to-instance state,
enabled-extension checks for these wrappers and live query-chain semantics
remain unfinished.

Fresh library/probe builds and runs `20260907T024657-4bc8a0d1` (29854870) and
`20260907T024657-607726a4` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. These results preserve the existing
headless baseline and do not close the live surface-query coverage gap.

## Wayland runtime dependencies

Wayland runtime closure (2026-09-07): a real window exposed the omitted
libwayland-egl.so.1 dependency. `tools/build-aarch64.sh` now recursively stages
DT_NEEDED dependencies of all installed ELF libraries/plugins, with explicit
interpreter/libpthread/libdl/librt roots. The cross sysroot takes precedence;
the interpreter hash changes accordingly, and unused libbsd/libmd are omitted.
Full X300 `20260907T080304-6c3bd550` remains **135 PASS / 10 UNSUPPORTED /
1 CRASH**; Redmi `20260907T080125-f9ffdff6` remains **98 PASS / 47 UNSUPPORTED /
1 CRASH**. Both only crash in native-groups; validation and capture/replay pass.
The separate [Wayland window probe](../wsi/README.md) now verifies an actual
fixed X300 window, including swapchain readback and ICC-aware screen pixels.
Redmi's current compositor lacks android_wlegl, so that optional gate is
UNSUPPORTED. This does not supply standard-loader WSI or presented-frame replay.
