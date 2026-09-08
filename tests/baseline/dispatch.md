# Vulkan dispatch probes

Registry coverage, entry routes, group enumeration and rendering dispatch. Dated runs retain their original failures and scope; they are not a current whole-suite pass claim.

## Registry and execution routes

`tools/registry/generate-dispatch-coverage.py` checks the SHA256 of Vulkan-Headers
v1.4.309 registry (`952f776f6573aafbb62ea717d871cd1d6816c387`) and generates
[command metadata](../../tools/registry/dispatch-coverage.json) plus the probe's
`dispatch_commands.inc`. Regenerate with:

    python3 tools/registry/generate-dispatch-coverage.py /path/to/vk.xml

Only commands provided by the Vulkan API are included (726); Vulkan SC-only
commands and disabled extensions are excluded. The metadata preserves aliases,
first parameter, scope, first core version and core/extension providers. It is
a query-coverage catalog, not a capability declaration or a feature evaluator.
The snapshot built into each probe includes the generated table's hash.

Each dispatch case writes `*-registry.json` with dlsym, GIPA(NULL),
GIPA(instance), GDPA(device) and linked-address availability for every name.
The probe verifies the 137 required core 1.0 entries and rejects non-global
commands returned by GIPA(NULL) (with GIPA's version-dependent self-lookup
exception), and non-device commands returned by GDPA. Existing specific
unenabled-extension checks remain. This is not every resolver rule or all
extension-enable combinations; later pointers are recorded but not executed.
See the [GIPA contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html)
and [GDPA contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceProcAddr.html).

`vk`, `vk-dlsym` and `vk-gdpa` run the same 4096-byte fill/fence/host-readback
workload. The linked binary's `vk` case now uses linked function addresses for
all workload calls; GDPA uses GIPA for instance/physical-device operations.
Earlier linked-vk results only established library dependency loading, since
the workload still called GIPA-derived pointers. Linked dispatch had separately
called create/destroy through linked symbols and retains that coverage.

`vk-core11` requests Vulkan 1.1; `vk-khr11` requests Vulkan 1.0 and explicitly
enables VK_KHR_bind_memory2 and VK_KHR_get_memory_requirements2. Both use their
corresponding GDPA aliases to query requirements and bind memory, compare
requirements against the 1.0 query, and complete the same GPU readback. Missing
versions/extensions report UNSUPPORTED. These cases do not claim complete
1.1 support. Neither connected device advertises dynamic rendering or
synchronization2, so their core/KHR execution remains unverified.

Verified runs: 29854870 `20260907T005630-74134b03` and KB2000
`20260907T005631-3e16a681`, each **45 PASS / 2 UNSUPPORTED**, including validation
and capture/replay. All five dispatch variants check 726 names with zero scope
errors. Native/hybris/standard-ICD dlsym availability was 232/632/269 commands;
GDPA returned 138 commands in each configuration. These are pointer-resolution
counts, not supported feature counts or a compatibility percentage.


Export split regression: rebuilt libraries and probes, compared all 643 defined
dynamic frontend exports (name/type/binding/visibility), then ran 29854870
`20260907T010200-b47ca796` and KB2000 `20260907T010201-038aaa84`.
Both are **45 PASS / 2 UNSUPPORTED**. All five registry-query reports per device
match their pre-split run exactly. The current fixed AArch64/Wayland-enabled
build is verified; this is not a new platform/build-configuration matrix.

## KHR physical-device groups

KHR groups and Redmi/X300 baseline (2026-09-07): `groups` and `groups-dlsym`
create fresh API 1.0/1.1 instances with KHR_device_group_creation enabled, use
KHR enumeration before any ordinary/core enumeration, query each returned
physical device and create/destroy a device with a usable queue. The wrapper
fix is verified on Redmi 29854870 and vivo X300 10AFA31610002QH. Old Redmi
frontend run `20260907T060046-41db0697` reproduces SIGSEGV via GIPA and abort
via its missing downstream ELF trampoline. Fixed targeted X300 run
`20260907T060105-6bc4ffc2` passes frontend GIPA/ELF and ICD GIPA. Vulkan's 643
export names are unchanged. Native and standard-loader KHR ELF symbols are
absent on these installations (UNSUPPORTED); native GIPA crashes remain CRASH.
The native X300 crash has x8=0x1cdc0de and faults in loader CreateDevice,
consistent with missing physical-handle initialization. The reference Android
loader source has core group SetData handling and no KHR group ProcHook:
https://android.googlesource.com/platform/frameworks/native/+/refs/heads/android10-release/vulkan/libvulkan/driver.cpp
This is supporting source evidence, not an assertion that the phone uses that
exact source revision.

Full Redmi run `20260907T060132-e4dca178`: 95 PASS, 4 UNSUPPORTED, 1 CRASH
(native-groups). Full X300 run `20260907T060132-4afa862a`: 79 PASS,
4 UNSUPPORTED, 16 CRASH, 1 FAIL. The latter includes frontend init/TLS, ICD
init and graphics pipeline failures, and ICD core11 transfer failure. Capture
and widget validation fail on X300's ICD path, while frontend widget passes.
These are open failures, not skipped cases or proof of a new regression against
an earlier X300 baseline. The system_ext search-path fix removed a separate
frontend startup failure (libgpud_sys.so not found).

For focused development use repeated `--case`, for example
`--case hybris-groups --case hybris-groups-dlsym`; selected names are recorded
in device.json. With no selection all cases run. `--capture-tools` can be combined with selected cases; the runner adds
`icd-version` and `icd-ubo` when needed and still executes both capture suites. Future device validation is restricted
to Redmi and X300; X300 APK installation must use `../../tools/install-apk.sh`
from the repository root (headless probes use adb push and need no APK).

## Dynamic rendering and Synchronization2

Dynamic rendering and Synchronization2 (2026-09-07): `render-core13` requests
API 1.3; `render-khr13` exercises the same promoted functionality through KHR
names on an API 1.1 instance/device. The latter enables dynamic_rendering,
synchronization2, depth_stencil_resolve and create_renderpass2. Both feature
bits are queried and enabled before creating the device. The fixed widget
uses Begin/EndRendering, PipelineBarrier2 for image transitions and host
visibility, and QueueSubmit2 plus a fence. The existing center-pixel oracle
checks both known descriptor bindings. GIPA and GDPA each run both bindings;
`-elf` and `-linked` variants exercise direct exports. The standard-loader
validation variants include synchronization validation. No unit-test suite
was added; render-path setup is kept in a separate source module.

Final full X300 `20260907T071924-d081b4de`: **110 PASS / 7 UNSUPPORTED /
3 CRASH** with the build-scoped Mali option enabled. Core13 and KHR proc-query
paths pass on native, frontend and ICD; core ELF/link paths also pass.
Both new validation cases pass. Frontend KHR ELF/link calls abort at
`vkCmdPipelineBarrier2KHR`, whose Android ELF implementation is absent;
native/standard-loader missing KHR ELF exports report UNSUPPORTED. The third
crash is the previously recorded native-groups failure. A candidate fallback
to core ELF trampolines still crashed (`20260907T071646-9a3a2d5d`) and was
removed; no alias runtime fix is included in this batch.

Final Redmi `20260907T071925-53a698f8`: **97 PASS / 22 UNSUPPORTED /
1 CRASH** (native-groups). All 18 new cases are unsupported because this
loader/device does not provide the required API 1.3 or KHR dynamic-rendering
capabilities. Existing widget validation and both capture/replay gates pass
on both devices. Those capture gates concern ordinary/dynamic UBO offsets,
not capture of the new dynamic-rendering command sequence. Multiview, depth,
resolves, secondary command buffers, semaphore dependencies, multiple queues,
non-coherent memory and window presentation are not covered by these cases.

## Rendering dispatch ownership

Frontend rendering dispatch fix (2026-09-07): KHR direct ELF/link calls now
resolve through the command buffer or queue's registered device, using the
exact backend GDPA name captured at device creation. The previous missing
`vkCmdPipelineBarrier2KHR` abort is fixed on X300; the earlier core-trampoline
fallback remains absent. Core/KHR GIPA/GDPA availability gates remain downstream
controlled. Vulkan's 643 exported names are unchanged.

`render-owners` holds two devices and queues obtained through GetDeviceQueue2
alive for six cycles. Each cycle mixes GIPA/GDPA/ELF dispatch, explicitly frees
and reallocates a command buffer, resets its pool, submits through KHR Submit2,
waits on a fence and implicitly frees remaining buffers through pool destruction.
`command-alloc` rejects pool metadata allocation and the second allocation in a
three-command-buffer batch, requires OUT_OF_HOST_MEMORY with no live-allocation
delta, recovers, and checks final callback allocation balance after teardown.
The follow-up sentinel-output runs `20260907T073033-e5e3003d` (X300) and
`20260907T073034-948a7fc7` (Redmi) also verify all three nonzero output slots
become NULL on batch failure, with final-live=0.

Full X300 `20260907T072903-d60ecde5`: **114 PASS / 7 UNSUPPORTED / 1 CRASH**.
Full Redmi `20260907T072904-613cf66c`: **98 PASS / 23 UNSUPPORTED / 1 CRASH**.
The remaining crash on each is native-groups. X300 uses the explicit scoped Mali
option for ICD cases. The four rendering commands pass via all frontend routes;
render-owners passes on X300 and is unsupported on Redmi's missing extensions.
Command allocation, existing lifecycle/concurrent device creation, widget
validation and both capture/replay gates pass. No unit-test suite was added.
Multi-GPU ownership, simultaneous command recording, protected queues,
secondary command buffers, arbitrary driver allocation failures and lookup
contention remain unverified; the metadata does not legalize stale-handle use
or concurrent destruction without Vulkan external synchronization.
