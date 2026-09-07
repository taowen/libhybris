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

The relative condition wait validates seconds/nanoseconds and checked deadline
addition, then uses CLOCK_MONOTONIC through the existing clockwait bridge.
It no longer normalizes invalid nanoseconds into a successful wait or relies
on a realtime absolute deadline. The clock probe covers normal 100 ms waiting
and invalid/overflowing durations; wall-clock jumps and shared waits remain
unverified. Reusing the clockwait path removes duplicate object translation.

The two Android cond_timedwait_monotonic aliases now call the clockwait bridge
with CLOCK_MONOTONIC, rather than interpreting uptime deadlines with the
condition variable's realtime default. A bionic fixture calls both aliases
with 100 ms deadlines and checks ETIMEDOUT after a non-immediate interval.
Shared conditions, clock jumps, relative deadlines and cancellation remain
outside this process-private timeout check.

The EGL lifecycle workload now draws with distinct blue/yellow fragment
programs in the isolated contexts after each clear. Current program and
vertex-attribute state persist across switches, thread migration and concurrent
worker loops without rebinding. Exact center pixels distinguish draw output
from the red/green clear checks. This adds independent threaded shader draws,
not shared-resource synchronization or proof of overlapping GPU execution.

Surface capability wrappers resolve their Android-loader ELF trampolines during
frontend construction rather than racing on lazy cache writes. Missing entries
return EXTENSION_NOT_PRESENT. The capabilities2 wrapper no longer falls back
to the legacy query, which cannot satisfy input/output pNext chains, and no
longer tries the invalid GIPA(NULL) scope for a physical-device command.
Physical-device-to-instance dispatch ownership and live WSI validation remain
open; this is not a complete per-object dispatch implementation.

The isolated EGL contexts also run on two threads: both make their context
current before a host gate releases their clear/readback loops. Each checks
its own context/surface, buffer binding/size and exact red or green pixel for
eight iterations, releases thread state, and returns to main for another
state check. This tests simultaneous current contexts, not guaranteed GPU
execution overlap, shared-resource hazards or concurrent shader draws.

The EGL lifecycle workload now also creates a share group of two GLES2
contexts. It checks shared buffer size changes, exact shared-texture pixels,
independent buffer binding state and continued access after destroying the
creating context. Three cycles use explicit completion before switching and
reattach the texture in the consumer context. Concurrent rendering and shared
object deletion while still referenced remain outside this workload.

`caps2` now compares six core 1.1 property structures as a chain and as
individual queries: ID, subgroup, point clipping, multiview, protected memory
and maintenance3. Core properties compare named fields against the legacy
query; LUID fields compare only when valid. The runner preserves features2/
properties2 backend differences separately in capability2-differences.json.
These queries do not prove shader behavior or supported resource sizes.

Swapchain creation now resolves the real function through the current device's
GDPA before preparing WSI. Wayland surface creation/destruction resolves through
the current instance's GIPA instead of caching the first instance's pointer;
destroy uses the proper void-return PFN type. `wsi-disabled` checks the
frontend's deliberate rejection policy for an unsupported direct-export call.
This is not a complete per-object dispatch table or working Wayland WSI proof.

Missing shared-memory backing now returns failure before the allocator or
translator touches the shared header. Mutex/condition/rwlock initialization
returns ENOMEM when no backing object is obtained. `shared-unavailable`
exercises actual bionic imports when glibc's /dev/shm is absent; this does not
implement Android ashmem support or validate working process-shared locks.
Shared rwlock destruction now translates its tagged handle before calling
glibc. Mutex timedlock and timeout_np likewise translate shared handles before
waiting, and these three paths return EINVAL when translation has no backing.
This is a code-path correction with build/private-path regression coverage;
working shared-object execution is not validated on the current devices,
which lack /dev/shm. The allocator's mapping/growth/concurrent-open behavior
still needs further work.

`common/bionic_sync.c` owns static synchronization publication, backing
allocation, mutex/condition/rwlock ABI hooks and shared-handle translation.
Its private header contains initializer values and cross-file declarations;
`hooks.c` retains symbol registration. Moving the 17 mutex/condition hooks
removed 541 lines, and moving the 15 rwlock/attribute hooks removed another
225 lines. Existing Android condition pulse and rwlock kind accessor exports
retain their ABI; newly cross-file hooks remain hidden. These structural
changes preserve the existing shared-memory behavior and its limitations.
The rwlock split passed runs `20260907T031532-6df22c8a` and
`20260907T031532-c897f4f0` (63 PASS / 2 UNSUPPORTED each), with the common
130-symbol and Vulkan 643-symbol defined export sets unchanged.
The mutex/condition split passed runs `20260907T030507-d17a9d9b` and
`20260907T030507-63e6a157` (61 PASS / 2 UNSUPPORTED each).

Destroying an unused process-private static mutex, condition or rwlock now
succeeds without treating its initializer as a glibc pointer. The independent
`sync-destroy` case covers normal/recursive/errorcheck mutexes, a condition and
a rwlock, then explicitly reinitializes the same storage and exercises another
valid lifecycle. Native and hybris execute the same bionic source. This does
not cover active-object destruction, double destroy or shared synchronization.

Rwlock kind attributes now translate bionic's reader/nonrecursive-writer
values (0/1) to glibc's corresponding policies (0/2), and translate queries
back. Values outside bionic's two-policy domain return EINVAL without changing
the attribute. `rwlock-kind` compares native and hybris defaults, round trips,
invalid values and ordinary lock lifecycle; it does not prove starvation or
fairness behavior under sustained reader/writer contention. Runs
`20260907T032015-6cdbdcb6` and `20260907T032015-9862e194` each complete
65 PASS / 2 UNSUPPORTED after rebuilding, including validation and capture.

Static condition-variable lookup and first allocation now use the same short
host publication guard as mutexes/rwlocks. Signal, broadcast and all four wait
wrappers share it, while actual waiting happens after the guard is released.
`cond-init` races a timed waiter with signal/broadcast on 32 fresh bionic
condition variables and releases a mutex-protected predicate. The old library
also passed this run; the race is established by code review, not a captured
lost wakeup. Shared conditions, alternate clocks and destruction with waiters
remain unverified.

Static rwlock first use now rechecks and publishes its backing pointer under
the same short host guard used for static mutexes. The independent `rwlock-init`
workload adds 32 fresh locks, four competing writers, four simultaneous readers
and try-write rejection while readers hold each lock. The pre-fix library
times out on this workload. This does not close condition-variable races,
process-shared rwlock behavior, timed operations or fairness requirements.

Vulkan platform loading and global function setup now each use pthread_once.
The module is published after init_module returns; null/Wayland global create
and enumeration pointers are resolved during setup instead of written lazily
by concurrent API callers. `vk-init` exercises simultaneous first global
operations and independent instance creation/query/destruction. This is an
initialization fix, not a per-instance/device dispatch or generation table.
The new workload exposed further first-use failures after the platform fix.
Static Android mutex translation now serializes backing-pointer lookup and
initialization, including four-byte-aligned bionic storage. A bionic DSO
workload checks 32 fresh static locks with four concurrent users. Two device
runs each complete 56 PASS / 2 UNSUPPORTED; archived pre-fix common times out
on the mutex workload, while the new common passes eight fresh processes.
Condition-variable lazy initialization, process-shared semantics and
lookup-lock performance still need separate work.

Promoted TLS registration now replays every entry missing from the calling
thread before advancing its initialization cursor. Previously a thread promoting
a later module could skip earlier modules registered by another thread. The
independent `tls-bounds` workload checks cross-thread initial values and that
catch-up preserves already-mutated TLS bytes. This addresses registry replay;
it does not provide slot reclamation, IE first-touch or signal reentrancy.

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
- common/hooks.c retains libc hooks and hook selection. bionic_tls.c owns the
  initial-exec TLS region, thread allocation and promoted-module replay registry.
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


EGL lifecycle batch: isolated GLES2 buffer/state and exact pbuffer pixels
survive repeated switches, main-to-worker-to-main context migration, and
three destroy/recreate cycles. Native and hybris pass on both devices in
`20260907T011354-4882a8fc` and `20260907T011355-938bfc04` (47 PASS /
2 UNSUPPORTED each; optional Vulkan tools not selected). Shared contexts,
parallel rendering, TLS destructor observation and generation tracking remain
open. The workload is an independent probe, with no production ABI changes.


Draw evidence batch: the fixed capture workload now checks draw-time UBO
content (272 bytes), descriptor set/buffer/range associations, attachment
before/after and attachment-to-copy identity. Both device runs
`20260907T011825-d445bf4a` and `20260907T011826-0c3dc4d9` complete 48 PASS /
2 UNSUPPORTED with capture, locating the injected fixture's pixel divergence
at draw 60, submit 66. This is one draw with capture-local IDs, not runtime
generation tracking or arbitrary application failure localization. G06/G12
remain partial/open at their wider scope.


Bionic TLS ownership split: moved the static TLS layout, per-thread compat
allocation, pthread cleanup keys and promoted-module registry out of hooks.c
into bionic_tls.c. hooks.c decreases from 3826 to 3549 lines. The two existing
linker/patcher callbacks retain their exported names/visibility. Fresh AArch64
library/probe builds preserve all 130 defined dynamic common exports and the
TLS segment's 1032-byte memory size, zero file size and 16-byte alignment.
Allocation, replay and destructor policies are unchanged. This is a structural
change, not proof of TLS destructor execution or resource reclamation.
Verified with optional VVL/SyncVal and capture on 29854870
(`20260907T012238-92e0cbe2`) and KB2000 (`20260907T012239-23559b33`):
50 PASS / 2 UNSUPPORTED each, including thread lifecycle, EGL migration and
fixed draw-resource/attachment checks.

TLS bounds fix: reject offset arithmetic wraparound, filesz > memsz and
nonzero copies from NULL before allocating or registering promoted TLS data.
Cleanup pthread key/create registration errors now fail explicitly. The old
library demonstrably accepted the wraparound input; the new independent child
probe checks four rejected inputs plus a legal zero-length end segment.
Fresh builds and both device runs (`20260907T012712-1e831443`,
`20260907T012714-d19f1840`) pass 51 cases with 2 UNSUPPORTED, including all
optional Vulkan tooling. Resource-failure injection and destructor observation
remain unverified; this does not close G03 as a whole.

TLS destructor/first-touch batch: real bionic C++ DSO fixtures cover emulated
TLS and ELF TLSDESC. The latter exposed nonzero initializers reading as zero
on glibc workers before any libc hook. Q static TLSDESC now initializes/replays
thread TLS while preserving resolver registers; its helper stays hidden and
plugin callback layout is unchanged. Both variants show the correct initial
value and one destructor on the owning thread after closing the main DSO
reference, across three cycles. Runs `20260907T013843-a5cf04e4` and
`20260907T013844-4eb30921` pass 52 / UNSUPPORTED 2, including optional Vulkan
tools. Per-access initialization overhead, signals/reentrancy, IE first-touch
and static slot reclamation remain unverified. G03 is still partial.


The optional standard-loader ICD now owns per-instance resolver/destructor
records with unique generations and bounded opt-in lifecycle diagnostics.
This establishes an instance ownership boundary without changing HAL handles
or input chains. Device/resource generations, the replacement frontend's
state and arbitrary draw/resource evidence remain open. Allocator failure,
custom-callback locking and malformed HAL behavior still need dedicated
coverage beyond the existing concurrent lifecycle workload.


The instance concurrent probe now holds every round's four successful instances
until all workers reach a barrier, then finishes all destructions before the
next round. Failure paths still reach both barriers; partial thread startup
cancels before entering them. The evidence checker now requires peak_live=4;
previous peak-2/peak-1 traces fail that stronger gate. This establishes actual
object overlap, not a guarantee of backend execution overlap or handle reuse.


Application allocation callbacks now have a native/frontend/ICD lifecycle
probe: three instance cycles return callback allocations to zero, followed by
an allocation-refusing create returning OUT_OF_HOST_MEMORY. Runs
`20260907T033851-afd02fc4` and `20260907T033851-da205be7` each complete
68 PASS / 2 UNSUPPORTED. Failure is measured at the API boundary; the loader
may stop it before ICD creation. Callback locking and individual ICD/HAL
failure sites remain unverified.


A separate `icd-alloc-direct` workload reaches the adapter's own instance-record
allocation via its exported ICD resolver. It refuses the first allocation,
requires exactly one callback and OUT_OF_HOST_MEMORY, then restores allocation
for three complete instance lifetimes. This isolates adapter failure/recovery
from the standard loader's earlier allocations; it does not exercise layer
chaining, every HAL failure site, allocator locking or device callbacks.


Direct ICD allocation failure coverage now also permits the adapter record
allocation before rejecting the next HAL allocation. Both tested devices
return OUT_OF_HOST_MEMORY after two callbacks, free the adapter record and
complete three recovery cycles. Runs `20260907T034553-3c4de9ce` and
`20260907T034553-5b6f5c0c` each complete 69 PASS / 2 UNSUPPORTED. Allocation
callbacks are prohibited from calling Vulkan commands; references to their
Vulkan reentrancy as an acceptance requirement were corrected. Other failure
positions and interaction with application allocator locks remain untested.


`common/bionic_stdio.c` now owns the bionic standard-stream storage/mapping,
78 compiled stdio/FILE hooks, wide-character streams, mount-table streams and
stdio buffer queries. `hooks.c` keeps the hook table and drops 695 lines to
2019. The private header shares the existing bionic offset types (also used
by mmap). Existing public `endmntent` ABI remains; newly cross-file hooks and
standard-stream storage are hidden. This moves existing implementations
without changing FILE/offset semantics or proving 32-bit stream ABI support.


Stdio flushing no longer probes fileno before fflush/fflush_unlocked. NULL
means flush all output streams, and memory streams have no descriptor but
still require flushing. The previous guard crashed for NULL and skipped
memory-stream updates. A bionic source fixture compares native and hybris
file-buffer flush-all/readback and open_memstream flush/content publication.
The unlocked entry receives the same correction but is not independently
executed by this fixture; broad stdio/FILE ABI coverage remains open.


Stdio position hooks now implement bionic's offset-only fpos via ftello/fseeko
(normal and 64-bit), instead of accessing glibc fpos internals. Failure no
longer copies uninitialized host position storage. Native/hybris probe checks
byte-stream save/restore and pipe ESPIPE with bionic position=-1. 32-bit
overflow, large-file boundaries and multibyte state remain unverified.


Widget rendering now has a dynamic-UBO probe with nonzero aligned descriptor
base and dynamic offset, deliberately different data in the other slots,
exact expected good/alternate pixels and an ICD validation/SyncVal variant.
That variant keeps an effective range of 272 bytes. Multiple dynamic
bindings and general dynamic descriptor history reconstruction remain open.


A second widget variant now uses a real 1232-byte shader block, with explicit
matrix array/column strides, signed int and 32-bit bool storage at the tail.
Shader checks of non-symmetric matrices and tail sentinels feed exact pixel
readback through ordinary and dynamic descriptors. This extends the independent
probe; arbitrary-application draw diagnosis and runtime generation remain open.


The staged widget probe now copies both UBO sizes into an unmapped device-local
buffer, retains its descriptor/resources, and checks six fenced submissions:
record/resubmit good, reset/re-record/resubmit alternate, then good again.
Transfer-to-shader and resource-reuse barriers are explicit. This adds bounded
staging and command-reuse evidence; template updates and arbitrary draw-state
reconstruction remain open.


The widget template probe now updates a retained descriptor set through Vulkan
1.1 core templates, using a nonzero payload offset with a valid opposite
prefix descriptor as a negative control. Both UBO sizes retain resources
across good/alternate/good updates and command re-record/resubmit cycles.
Multiple entries, descriptor arrays and dynamic capture reconstruction remain
open.


GFXReconstruct capture integration now has an independent dynamic-widget
case. Its evidence checker matches the dump's effective offset to descriptor
base plus bound dynamic offset and verifies complete UBO bytes, attachments
and final copy pixels. This extends fixed-fixture diagnostics; general draw
state reconstruction, template histories and runtime generations remain open.


Captured widget pipeline evidence now includes actual API-input shader
binaries, validated against the probe build snapshot, plus SPIR-V validation,
disassembly/decorations and pipeline/layout/module associations. A dedicated
shader_evidence module keeps those checks separate from draw-resource checks.
Driver-internal shader transforms/cache keys and general pipeline histories
remain unobserved.


Widget UBO layouts and data construction now live in widget_fixture.h. The
draw routine consumes owned good/alternate data and a byte count, leaving
resource setup, upload, recording and readback in probe_widget.c. Small-layout
size/offset assertions join the existing large-layout checks. This is a
maintainability change; generation and general draw-history gaps remain open.


Attachment evidence now separately checks successful image/view/framebuffer
creation, render-pass attachment selection and copy-source identity, including
view/copy subresource and dimensions. Recorded bind/draw/copy commands must
belong to the submitted command buffer. This remains a fixed one-image
fixture, not runtime resource generations or WSI lineage.


Mutex destroy now preserves backing allocation and bionic storage on host
failure (including EBUSY), and clears/frees only on success. A failed shared
translation returns EINVAL. The independent bionic fixture checks three mutex
types for EBUSY preservation and subsequent unlock/relock/destroy. Successful
shared backing and concurrent destruction remain unverified.


Legacy mutex millisecond timeout now uses an explicit monotonic host deadline
and maps timeout to bionic EBUSY. The fixture imports the hook and exercises
zero-timeout acquisition, a 100ms failure and reuse. LP64 native comparison,
clock-step behavior, 32-bit overflow and process-shared operation are unverified.


The API-28 mutex monotonic timedlock symbol is now hooked. Realtime and
monotonic absolute timedlock share backing-mutex translation and use explicit
clock IDs; the legacy relative-millisecond entry retains its EBUSY mapping.
The native/imported-fixture probe checks timeout and expired-deadline
acquisition on an unlocked mutex, including a null deadline. Null deadlines
use ordinary blocking lock. PI/shared/time-jump coverage remains open.


Android monotonic timed read/write rwlock imports now have explicit hooks,
using existing backing-object translation and host monotonic clock waits.
Null deadlines use blocking operations. Independent bionic/native probes
check cross-thread read/write timeouts and unlocked expired/null acquisition;
shared semantics, fairness and runtime generation remain open.


Hook registry sorting uses pthread_once instead of an unsynchronized static
flag. Concurrent first lookups therefore cannot qsort/search the same mutable
table simultaneously. The callback still runs before table initialization.
Existing concurrent linker initialization and GPU cases provide regression
coverage, not an isolated first-sort race reproduction. The internal lookup
is not exported. Mutable lookup configuration and diagnostics are outside
this change.


Hook callback publication now uses release/acquire atomics and each lookup
keeps one local callback snapshot. Missing pthread-hook diagnostic IDs use
atomic decrement, and the unhooked-log option is initialized with pthread_once.
Replacing a callback does not wait for an in-flight invocation; code/context
lifetime remains caller-owned. This removes these specific shared-state races
without claiming global lookup/configuration thread safety.


The optional ICD now owns device records in `icd/device.c`, with a unique
per-device generation, parent instance generation, backend GDPA/destructor
and allocator. Instance-local physical inventories are populated by ordinary
and core/KHR group enumeration, including returned VK_INCOMPLETE entries.
Creation forwards the original create info and callbacks. GIPA/GDPA only
substitute lifecycle wrappers when the backend exposes the queried command;
ordinary device commands retain the current backend's lookup result.
Registry locks never enclose driver or allocation callbacks. Vulkan external
synchronization remains required; these tables do not make stale handles or
concurrent destruction valid. Metadata retirement precedes backend destruction.

The life runner validates device trace pairing and live instance parents.
The direct allocator probe checks a rejected first device-record allocation,
recovery and final allocation balance through ordinary/core/KHR physical-device
enumeration. Android-loader KHR group handle use crashed in both native and
replacement-front-end exploratory runs; the final non-direct allocator probe
uses ordinary enumeration. This unresolved path is recorded in gaps.md.
Resource state, multi-GPU ownership, physical-inventory OOM and arbitrary
allocator failures remain outside this evidence.


KHR physical-device group lookup in the replacement frontend now retains the
backend KHR availability gate, then prefers the loader's core group wrapper.
Some Android loaders forward the KHR name to the HAL without initializing
returned physical-device dispatch headers; subsequent loader CreateDevice can
read the ICD magic as a pointer. GIPA and the direct ELF KHR symbol share this
wrapper. When no core wrapper exists, the original KHR command is retained;
that fallback is not covered on current devices. No handle header is modified.
The AArch64 build adds system_ext/lib64 for modern Android loader dependencies.
See gaps.md's Redmi/X300 checkpoint for passing paths and unresolved Mali
pipeline/concurrency failures; the full X300 baseline is not passing.


AArch64 patched MRS reads now initialize foreign-thread bionic TLS on first
touch through `tls_first_touch.S`; integer/NEON registers and flags are saved
around allocation and host errno is restored. The pthread shadow includes
the tid prefix needed by inlined bionic mutex paths. The fast path only checks
slot 1, so late module initializer replay remains the hook/TLSDESC resolver's
responsibility. Reservation and allocation use HYBRIS_TLS_THUNK_SIZE (80).
The first-touch probe and full Redmi/X300 results are in the baseline README;
X300 initialization/TLS passes, but ICD graphics pipelines still crash.
Signal/fork, SVE/SME and arbitrary Android pthread layouts are not covered.


The standalone widget probe now shares dynamic-rendering and Synchronization2
setup in `tests/baseline/render_path.c`. It explicitly queries/enables features,
uses either API 1.3 core names or API 1.1 with the KHR extensions and their
promoted dependencies, and exercises begin/end rendering, image/host barriers
and queue submit2. GIPA, GDPA, ELF dlsym and linked weak symbols are separate
routes. Missing optional ELF exports report UNSUPPORTED; exported frontend
stubs that abort remain CRASH. See the baseline README for device results.
The readback oracle checks the existing widget's center pixel for both known
UBO bindings; these cases do not establish arbitrary shader/rendering semantics
or capture/replay of dynamic rendering. The existing capture-dynamic workload
means dynamic UBO offsets, not dynamic rendering.


The replacement frontend now owns device/queue/pool/command-buffer metadata in
`hybris/vulkan/render_dispatch.c`. Device creation reserves queue slots from
the supplied queue create infos and caches eight GDPA results for core/KHR
BeginRendering, EndRendering, PipelineBarrier2 and QueueSubmit2. Queue getters
and command-buffer allocation register the returned handles. The direct ELF
functions and backend-available GIPA/GDPA wrappers use those object-specific
results; there is no core-name fallback and no dispatch-header inspection.
Pool destruction retires implicit command-buffer records; explicit buffer free
and device destruction retire their records. Pool reset retains valid records.

Metadata uses the corresponding device/pool allocation callbacks. Allocation
failure unwinds a pending command-buffer batch and clears every output handle.
Driver calls and allocation callbacks execute outside the registry mutex.
Vulkan external synchronization remains required. Handles created by bypassing
the frontend are outside this registry's contract. The table uses linear lookup
under one mutex; draw-path contention/performance is not established. This is
not a complete frontend resource registry, generation-based diagnostic system,
or an implementation of all promoted-command aliases. Building this module
requires Vulkan 1.3 headers, as supplied by the current AArch64 build toolchain.


Host timeline semaphore commands live in `timeline_dispatch.c`. Their core/KHR
ELF exports resolve the exact backend name using the supplied registered
VkDevice. GIPA/GDPA only return these wrappers after the backend availability
gate succeeds. Lookup releases the metadata mutex before invoking GDPA or the
command, so a host wait does not hold that mutex against a signal from another
thread. Missing implementations return an error without modifying output data.
This forwards native timeline support; it neither emulates timeline semaphores
nor changes advertised feature bits. Semaphore objects are not independently
tracked by this dispatch helper.


The cross-build now stages the transitive DT_NEEDED closure of all installed
ELFs, including platform plugins, through `tools/stage-runtime.py`. Explicit
roots retain the glibc interpreter and compatibility libpthread/libdl/librt
DSOs. Search uses the AArch64 cross sysroot first, then target package library
directories; missing dependencies fail the build. This adds the previously
omitted libwayland-egl.so.1 and avoids staging unused libbsd/libmd. The interpreter
now comes from the same first-choice cross sysroot as libc; its recorded hash
changed and the full device regressions were rerun. Android libraries loaded
by the separate linker remain runtime mapping/hash evidence, not part of this
DT_NEEDED closure.

`tests/wsi` independently builds/runs a Wayland Vulkan window in the existing
compositor app's UID. It retains full first/final swapchain readbacks and real
screenshots, checks the fixed window's expected color transition through the
embedded screenshot profile, and records compositor APK and mapped library
identities. See its README for the separate submission, callback and display
checks. This does not add standard-loader ICD WSI or establish buffer-release,
resize or application-level correctness.


Wayland Vulkan surface-map lookup, insertion and removal now share a mutex.
Destruction removes a matching entry in one locked operation, then releases the
lock before querying/calling the driver or touching native-window/Wayland state.
Lookup returns a borrowed pointer; Vulkan external synchronization still governs
same-surface use versus destruction. This protects distinct surfaces from map
mutation races, not arbitrary stale-handle calls. The independent WSI lifecycle
helper exercises four threads and checks warmed client FD balance before the
existing displayed-window gate. It does not establish allocator cleanup or
compositor-side FD balance.


Wayland surface discovery now completes a roundtrip on its private event queue
before constructing the native window. Missing android_wlegl or a failed
discovery roundtrip returns VK_ERROR_UNKNOWN with a diagnostic instead of
aborting in a sync callback. This command does not list INITIALIZATION_FAILED;
UNKNOWN is the general unexpected-error result described in the
[Vulkan return-code rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkResult.html).
Partial discovery allocations are checked and cleaned without further dispatch.
A backend surface-creation failure also destroys the temporary wl_egl_window.
The independent probe exercises repeated missing-protocol rejection on Redmi
and the existing successful concurrent surfaces and window on X300. Allocation
failure, display disconnect, vendor creation failure and heap reclamation have
not been fault-injected; those branches are code-reviewed only.


Vulkan Wayland reconnect now retires the old producer buffer pool. Android
Vulkan disconnects/reconnects when recreating a used native-window swapchain
([AOSP implementation](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/vulkan/libvulkan/swapchain.cpp)).
The previous default no-op left the currently displayed buffer unavailable,
blocking replacement allocation. The Vulkan override drops its references to non-displayed pool buffers
and retains displayed ones in the existing fronted list until Wayland release.
A retired release frees its reference without changing the new pool's free
count; window teardown also cleans remaining retired proxies. Other backends
keep the default disconnect behavior. Rebuild the complete C++ platform bundle
for the added virtual hook/private helper signature; Vulkan's 643 exported
names are unchanged. Tests/wsi now verifies same-window 320x240 → 448x288 →
256x192 oldSwapchain replacement and actual screen pixels at each size. The
separate compositor GPU binding-table exhaustion indicated by source review
and runtime recovery after a restart remains open,
as do in-flight/failure recovery and compositor-driven resize semantics.


The standalone builder has an opt-in incremental compiler cache. Preparation
and cache publication live in tools/prepare-build.py; inputs/ records the clean
source snapshot separately from generated src/ objects. C/C++/assembly changes
are synchronized by content, with refreshed mtimes; conservative header/rule/
configuration/file-set changes trigger a fresh build. Make visits the complete
dependency graph, and install/runtime are always recreated before manifest
publication. A nonblocking per-output lock prevents concurrent mutation.
The manifest records clean/incremental mode; build-report.json records the
actual decision and elapsed time. Failed preparation withdraws prior completion
markers. This accelerates the edit/build loop but does not replace clean-build
regression acceptance or provide a tamper-proof compiler cache.

WSI diagnostics are isolated in tests/wsi/diagnostics.py. A bounded PID-filtered
compositor log accompanies each run; stalls trigger read-only client/compositor
process snapshots before the owned client is terminated. Collection errors,
limits and collector hash are recorded independently of image acceptance.
This improves first-failure evidence but is not an isolated compositor, a GPU
trace or a debugger backtrace. See tests/wsi/README.md for the actual old-library
stall and normal-window verification and remaining limitations.

Tests/wsi/compositor provides an independent debug APK and per-run process
wrapper. The small Java/JNI host supplies a real Android Surface to anlabwc;
its native dependency closure and xkb assets are imported from a caller-selected
APK with recorded hashes. The separate package/UID and fresh process remove
cross-run native-state accumulation without restarting Ardesk. Both X300 and
Redmi now pass the same three-size screen gate on this chosen backend. This is
not a source-built compositor or a fix for long-lived backend resource leaks.
