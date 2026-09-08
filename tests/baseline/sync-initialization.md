# Bionic synchronization ownership probes

First use, backing-object publication, static destruction, rwlock kinds and allocation ownership. [Timeout and clock semantics](sync-timeouts.md) are documented separately. Results below are dated evidence with their original coverage limits.

## Concurrent first Vulkan instance operations

`vk-init` loads the frontend, resolves its ELF exports, then releases four
workers from a condition-variable gate without making a Vulkan API call on
main. Two workers first create an instance and two first enumerate global
extensions. Each performs four create/physical-device-count/destroy cycles,
using both ELF and GIPA create routes. Failed thread creation cancels and joins
every started worker. Native, hybris and standard-loader ICD run this workload.

Code review found unsynchronized publication of the platform module before
its initializer returned and lazy writes to global create/enumerate pointers.
The frontend now serializes module initialization and proc setup with separate
pthread_once controls. Null and Wayland plugins resolve global pointers during
that setup. Missing global entries return initialization failure. The plugin
initializer must not reenter the frontend's platform API while its once is
running; the current initializers call gralloc/common setup only.

This workload does not deterministically reproduce the old data race and is
not a ThreadSanitizer result. The headless runs exercise the null platform;
Wayland changes are build-checked only. Surface maps, surface/swapchain function
caches, per-object dispatch and generation tracking remain separate work.

The platform-only change still failed this workload on 29854870
(`20260907T015316-00d0a8ae`, incomplete worker cycles) and KB2000
(`20260907T015316-458817a0`, watchdog timeout), while native and ICD passed.
Further review found that the pthread bridge could allocate multiple backing
mutexes for the same static Android mutex and overwrite its pointer. A thread
could then unlock a different mutex from the one it acquired. The bridge now
guards pointer lookup/publication with a short host mutex, released before
acquiring or waiting on the translated mutex. It rechecks before allocating.
Four-byte-aligned bionic storage requires memcpy; a discarded pointer-atomic
implementation triggered SIGBUS and is not part of the final change.

`mutex-init` calls real pthread imports in the bionic fixture DSO. Four threads
race to first use each of 32 static mutexes; an atomic occupancy counter checks
mutual exclusion, all workers join, and every mutex is destroyed. The fixture
asserts its mutex offset is four bytes to exercise Android's alignment. This
does not touch its TLS object. It uses the existing fixture build/manifest.

Fresh builds and runs `20260907T020119-b9cd6fa2` (29854870) and
`20260907T020119-e59eacf4` (KB2000) each complete **56 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. All Vulkan workers complete four
cycles on native/hybris/ICD; all mutex workers report zero overlap/errors.
Vulkan's 643 and common's 130 dynamic exports remain unchanged.

A separate 29854870 comparison reuses the first run's staged probe/fixture and
dependencies, replacing only the common library: archived pre-fix common
SHA256 `f9a90d389d254959e663e93d0fa81eb817b3d084fbcc0466d0c0a124c087df43`
times out (exit 142); final common
`a77a96a95056c483deb8b934a5d61bb5b0dd3a12a74d10afcfd41927f017e42c`
passes eight fresh-process repetitions. This is a mixed-artifact callback
regression comparison, not a full old-release baseline.

Remaining limits: condition-variable/rwlock lazy initialization still needs
review, process-shared behavior is unchanged, and lookup-lock performance,
fork/signal reentrancy, timed/recursive/errorcheck mutex semantics and allocation
failure injection are not validated by this normal-mutex workload.

## Static rwlock first use and reader sharing

`rwlock-init` reuses the lock workload in `probe_lock_init.c` with real rwlock
imports from the bionic fixture. Four workers race to write-lock each of 32
fresh static rwlocks; atomic occupancy must remain one inside every critical
section. Then all four workers read-lock each rwlock simultaneously and meet
at a barrier before any releases it. While all four readers hold the lock,
trywrlock must return EBUSY. Every worker joins and every rwlock is destroyed.
Thread creation failure releases and joins all started workers.

Before the fix, run `20260907T020611-1e79682d` on 29854870 times out in
`hybris-rwlock-init` (watchdog exit 142). Multiple threads could allocate and
publish different backing locks for one Android static initializer. The bridge
now rechecks and publishes under the existing host synchronization guard and
uses that guard when reading the pointer for unlock. It releases the guard
before acquiring or waiting on the user's rwlock. Allocation/init failures
produce explicit fatal diagnostics; those failure paths are not injected.

This covers process-private static write first use and subsequent read sharing.
It does not establish writer fairness, timed/try-read behavior, process-shared
rwlocks, fork/signal reentrancy, allocation cleanup on failure or lookup-lock
performance. Condition-variable lazy initialization remains separate work.

Fresh library/probe builds and runs `20260907T020805-a5abffff` (29854870) and
`20260907T020805-1a903c3d` (KB2000) each complete **57 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. All four rwlock workers report zero
errors on both devices; the prior mutex and concurrent Vulkan workloads also
pass. The common library retains the same 130 dynamic exports.

## Static condition-variable initialization and wakeup

`cond-init` uses 32 fresh, zero-initialized bionic condition variables with
four-byte alignment. A timed waiter and two threads calling signal/broadcast
start together. Main changes the predicate under the same bionic mutex that
the waiter uses, then broadcasts; the early signal/broadcast calls may cause
spurious wakeups. The waiter loops on the predicate with a 500 ms absolute
realtime deadline. Every wait/pulse must succeed, all threads join, and all
condition variables and mutexes are destroyed. No condition is destroyed while
a waiter is active. Failed worker creation cancels and joins started workers.

The old common library passed `20260907T021201-1f959389` on 29854870. This is
not a deterministic reproducer of lost wakeups. Code review found that signal,
broadcast and wait wrappers could concurrently allocate different backing
condition variables and overwrite one another's pointers. Lookup and first
allocation now share the existing host publication guard. Actual wait/signal
operations run after it is released. Initialization failures now have explicit
fatal diagnostics; allocation-failure paths are not injected.

This probe exercises timedwait's normal wakeup path, signal and broadcast. It
does not validate plain wait, explicit/monotonic clocks, relative deadlines,
expected timeout behavior, process-shared conditions, waiter cancellation,
destruction with waiters, allocation reclamation or guard performance. The
existing destroy hook's modification of glibc waiter metadata is unchanged and
must not be inferred safe from this legal teardown workload.

Fresh builds and runs `20260907T021359-9aa5efda` (29854870) and
`20260907T021359-51c1edc4` (KB2000) each complete **58 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. All three condition workers report
zero errors on both devices. Common's 130 dynamic exports remain unchanged.

## Synchronization allocation ownership split

`common/bionic_sync.c` owns the host publication guard, static mutex/condition/
rwlock allocation and pointer lookup. `bionic_sync.h` holds initializer values
and four hidden helper declarations. `hooks.c` keeps the API wrappers, explicit
initialization/destruction and shared-memory handle translation. Its size falls
from 3588 to 3480 lines; the new implementation file is 131 lines.

The extracted helper bodies match the previous source apart from internal
linkage. Rwlock pointer publication moves behind one private helper; shared
handle translation still occurs afterward. No allocation policy, locking
scope, condition clock or destruction behavior changes in this split. The
library's 130 dynamic exports match by name, type, binding and visibility;
none of the new cross-file helpers is exported.

Fresh library/probe builds and runs `20260907T021854-5ecc07d9` (29854870) and
`20260907T021854-fb23cbb2` (KB2000) each complete **58 PASS / 2 UNSUPPORTED**,
including the three synchronization first-use probes, VVL, SyncVal and
capture/replay. This structural change adds no compatibility coverage beyond
those existing workloads; their documented limitations still apply.

## Missing shared-memory backing

The hybris-only `shared-unavailable` case requires glibc's `/dev/shm` directory
to be absent; it reports unsupported if that precondition is not met. It
checks that the existing shared allocator returns zero and translation of an
AArch64 tagged offset returns NULL. It then calls actual bionic pthread
mutex/condition/rwlock attribute and initialization APIs through the fixture:
PROCESS_SHARED initialization must return ENOMEM for all three object kinds.
It does not destroy failed objects or exercise them as initialized locks.

Before the fix, `20260907T022257-f825a071` on 29854870 crashes with SIGSEGV
(exit 139) on the allocator call. Allocation and translation now check that
the backing store was opened before accessing its header. The three pthread
initializers propagate missing allocation/translation as ENOMEM instead of
passing NULL to glibc. This also guards a NULL private allocation, but malloc
failure is not injected by this workload.

This is verified failure handling, not process-shared lock support. It adds
no ashmem/memfd backend and does not validate successful shared mappings,
concurrent initialization, growth/remap, interprocess exclusion, shared-object
destruction or allocation reclamation. Existing shared rwlock destruction and
condition-wait semantics remain separate defects to address.

Fresh library/probe builds and runs `20260907T022445-38be09a4` (29854870) and
`20260907T022445-d0261787` (KB2000) each complete **59 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Each device returns ENOMEM (12) for
all three shared initializers. Common's 130 dynamic exports remain unchanged.

## Mutex and condition hook split

Mutex/condition hook split regression: `bionic_sync.c` now owns the 17 API
hooks and Android condition pulse helpers in addition to static publication.
`hooks.c` keeps the unchanged registration table. Compared the moved bodies
and registration against the preceding revision; rebuilt libraries and probes.
All 130 common and 643 Vulkan defined dynamic symbols remain unchanged, and
the 17 cross-file hook entry points remain private.
Runs `20260907T030507-d17a9d9b` (29854870) and
`20260907T030507-63e6a157` (KB2000) each completed **61 PASS / 2 UNSUPPORTED**,
including mutex/rwlock/condition first use, condition clock/error handling,
shared-allocation failure, EGL/Vulkan, validation/SyncVal and capture/replay.
This structural change does not establish working Android process-shared
synchronization, destruction with waiters, 32-bit ABI coverage or fairness.

## Unused static destruction

`sync-destroy` checks five unused private static initializers: normal,
recursive and errorcheck mutexes, a condition and a rwlock. Each is destroyed,
explicitly reinitialized in the same storage, used and destroyed again.
`sync_fixture.h` supplies identical bionic lifecycle code to the native probe
and the Android DSO loaded by hybris. It is included in the probe manifest.
The old common library returned EINVAL for the first normal mutex in
`20260907T030830-362e5220`; that run's native fixture load did not reach the
workload and is not native semantic evidence. The final native probe compiles
this lifecycle directly, avoiding unrelated TLS fixture imports.
After rebuilding, runs `20260907T031048-dcf57325` (29854870) and
`20260907T031048-d72e6be0` (KB2000) each completed **63 PASS / 2 UNSUPPORTED**,
including native/hybris success for all five kinds, validation/SyncVal and
capture/replay. The 130 common and 643 Vulkan defined exports are unchanged.
No claim is made about double destroy, destruction while in use, 32-bit ABI,
working shared synchronization or reclamation of the shared allocator.

## Rwlock hook split

Rwlock hook split regression: moved 15 rwlock/attribute hooks and their handle
lookup from `hooks.c` into `bionic_sync.c`. The two existing public kind
accessors remain exported; other moved entry points are hidden. Bodies and
registration were compared to the preceding revision; all 130 common and
643 Vulkan defined exports remain unchanged after rebuilding.
Runs `20260907T031532-6df22c8a` (29854870) and
`20260907T031532-c897f4f0` (KB2000) each completed **63 PASS / 2 UNSUPPORTED**,
including rwlock first use, static destruction/reinitialization, TLS, graphics,
validation/SyncVal and capture/replay. This is structural regression evidence;
shared rwlock destroy translation, timed-lock semantics, kind preferences and
fairness are not newly verified by the existing first-use workload.

## Rwlock kind translation

`rwlock-kind` checks bionic rwlock attribute default, both supported kind
round trips, lock creation/read/write/destruction for each kind, and EINVAL
for -1, 2, 3 and INT_MAX without changing the last valid kind. It shares the
bionic lifecycle source between the native probe and the DSO used by hybris.
Bionic's nonrecursive-writer enum is 1, while glibc's matching enum is 2;
passing values through selected a different host policy and accepted the
bionic-invalid 2. See the [AOSP attribute implementation](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r3/libc/bionic/pthread_rwlock.cpp).
Before the fix, `20260907T031839-af08782d` passed natively and failed through
hybris because setting 2 returned success instead of EINVAL. The bridge now
maps both directions explicitly and rejects the host-only policy domain.
The workload does not establish starvation freedom, scheduling order,
recursive-reader misuse behavior, timed locks or cross-process synchronization.
After rebuilding libraries and probes, `20260907T032015-6cdbdcb6` (29854870)
and `20260907T032015-9862e194` (KB2000) each completed **65 PASS / 2 UNSUPPORTED**,
including native/hybris kind cases, validation/SyncVal and capture/replay.
The 130 common and 643 Vulkan defined dynamic export sets remain unchanged.

## Shared-handle translation

Shared-handle correction: mutex timedlock/timeout_np now translate a hybris
shared-memory tag before passing it to glibc; shared rwlock destruction now
translates before destroy rather than afterward. Missing translation returns
EINVAL in these three paths. No shared-memory allocator, Android-native shared
mutex no-op policy, or condition wait policy is changed.
Current devices lack /dev/shm, and no ready proot binary was available in the
workspace. The existing `shared-unavailable` case covers allocation/translation
failure and failed initialization, not these successful shared-object paths.
This change therefore has source review, build and existing private-path
regression evidence; working process-shared timed waits/destruction remain
unverified, as do allocator growth/remapping and cross-process coordination.
Rebuilt libraries/probes and completed runs `20260907T032439-4c9cf97d`
(29854870) and `20260907T032439-0e18b5ae` (KB2000), each **65 PASS /
2 UNSUPPORTED**, including validation/SyncVal and capture/replay. The 130
common and 643 Vulkan defined exports remain unchanged. These counts do not
close the shared-path coverage gap described above.

## Busy mutex destruction

`sync-destroy` additionally checks busy normal/recursive/errorcheck mutexes.
The native/bionic fixture requires EBUSY with unchanged mutex storage, then
unlocks, locks/unlocks again and destroys successfully. This follows bionic's
explicit busy-destroy behavior, not a general POSIX portability guarantee.
Before the fix, `20260907T044320-8811de0f` reports native PASS and hybris FAIL:
the ordinary mutex returned EBUSY but its backing pointer was cleared. The
probe stops before touching discarded storage on that failure.
The hook now frees/clears only after successful host destruction and rejects
a failed shared-handle translation. Working process-shared mutex destruction
and concurrent destruction are not covered by this probe.
Source: https://android.googlesource.com/platform/bionic/+/master/libc/bionic/pthread_mutex.cpp

Rebuilt runs `20260907T044459-7e3295ea` (29854870) and
`20260907T044459-327bc0c1` (KB2000) each report 88 PASS, 2 UNSUPPORTED,
including validation/SyncVal and both capture gates. All three native/hybris
mutex types return EBUSY=16 with preserved=1, followed by successful
unlock/relock/final destroy. Common's 130 defined dynamic exports are unchanged.
The old comparison stops at the first failed normal mutex; it does not
separately reproduce old recursive/errorcheck behavior.

## Condition-variable production split (2026-09-07)

`hybris/common/bionic_cond.c` now owns condition init/destroy, signal/broadcast,
wait/clockwait, monotonic/relative aliases and Android futex wake helpers.
`bionic_sync.c` retains the single publication lock shared by conditions and
mutexes, plus mutex/rwlock hooks; it shrinks from 1003 to 660 lines. The shared
Android mutex predicate moves to the private header as an inline helper.
Condition hook and futex helper bodies were checked verbatim against the
original source. The central hook table and private declarations are unchanged.

A clean `tools/build-aarch64.sh` build succeeds. The defined dynamic symbol
names, types, bindings and visibility of libhybris-common match the previous
build (133 entries); condition hook entry points remain hidden. The existing
probe binaries exercise the newly built library without changing workloads.

| Device | Result directory | Results |
|---|---|---|
| X300 / Mali | `20260907T155138-14652fa8` | 19 PASS |
| Redmi 29854870 / Adreno vendor | `20260907T155138-efb59546` | 19 PASS |

Cases include cond-init/cond-clock, mutex/rwlock first publication, static
synchronization destruction, concurrent linker/Vulkan initialization, TLS,
unload, device lifecycle, EGL context lifecycle, GLES3 and UBO rendering.
Native sync-destroy/egl-life/vk-init provide comparisons. Standard ICD
validation retains its expected invalid-buffer diagnostic; the legal UBO
validation/SyncVal path also passes. Both runners verify freshly staged
library provenance and required mappings. Mali retains the scoped loader
quirk. Local `build/cond-split/audit.json` records symbol and source-body checks.

This is a responsibility split, not a new synchronization implementation.
Existing Android-shared wait branches and the glibc-private `__wrefs` handling
at destruction are preserved; shared waiting and destruction with live
waiters are not validated or fixed by this batch. G03 remains open.
