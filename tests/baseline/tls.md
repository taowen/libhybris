# Bionic TLS probes

TLS bounds, compiler-generated destructors, module initialization and foreign-thread first touch. Dated records describe the tested revisions and retain the original failures; they do not prove arbitrary Android TLS compatibility.

## TLS implementation split

Bionic TLS source split regression: fresh library/probe builds preserve 130
common dynamic exports (name/type/binding/visibility) and PT_TLS file size 0,
memory size 1032 bytes, alignment 16. Runs `20260907T012238-92e0cbe2`
(29854870) and `20260907T012239-23559b33` (KB2000) each complete
**50 PASS / 2 UNSUPPORTED**, with VVL, SyncVal and draw capture evidence enabled.
No new claim of actual TLS destructor execution follows from this source split.

## Promoted TLS bounds

The hybris-only `tls-bounds` probe calls the existing linker callback in
isolated child processes, without loading Android/GPU libraries. Offset
addition wraparound, filesz larger than memsz, memsz beyond the static area
and a NULL source with nonzero filesz must each terminate with SIGABRT.
A zero-length segment at the exact end remains accepted. Core dumps are
disabled in these children; expected aborts are checked by the parent, not
classified as successful GPU calls or hidden as unsupported results.

Before the fix, run `20260907T012533-871fb7f5` failed the wraparound rejection:
the callback accepted SIZE_MAX + 2 because its addition wrapped. The fix checks
offset and remaining space by subtraction, and validates filesz/source before
allocation, copying or publishing to the registry. Cleanup key creation and
setspecific failures now abort with an explicit diagnostic instead of silently
losing ownership. Key exhaustion/setspecific failure injection remains untested.

Fresh library/probe builds preserve 130 common dynamic exports. Final runs
`20260907T012712-1e831443` (29854870) and `20260907T012714-d19f1840` (KB2000)
each complete **51 PASS / 2 UNSUPPORTED**, including VVL, SyncVal and draw
capture evidence. All four invalid-range children and the legal empty-end
case pass. This does not prove general ELF parsing safety, TLS destructor
execution or static TLS slot reclamation.

## Compiler-generated TLS destructors and first touch

The hybris-only `tls-dtor` case loads a bionic C++ DSO with a `thread_local`
object. Two builds use NDK's default emulated TLS and `-fno-emulated-tls` ELF
TLS respectively. The second ELF has PT_TLS (24 bytes initialized, 25 bytes
reserved) and AArch64 TLSDESC relocations. Both fixture binaries, source and
build command identity are included in the probe manifest and verified while
staging. The fixture intentionally imports __cxa_thread_atexit to exercise the
hybris hook and is not run as a native Android-loader case.

A glibc-created worker first touches the object, reads initial value 73 and
sets 1234. Main drops its Android DSO handle while the worker is paused; after
release and join, the callback must have run exactly once on the worker with
1234, and must not have run before thread exit. Three load/thread/close/join
cycles cover each variant. This observes actual C++ destructor execution,
not just a successful thread join or dlclose return.

Before the fix, both devices failed the ELF TLS variant with initial value 0:
`20260907T013303-3f85d4f7` and `20260907T013304-05719b7c`. The old static
TLSDESC resolver returned an offset without initializing a glibc thread that
had not called any bionic libc hook. Q linker's static resolver now calls the
TLS initializer before returning its offset, preserving GPR and vector
registers across that call. The callback is resolved during linker setup;
new helper symbols are hidden. This does not change the linker plugin callback
struct ABI.

Final fresh library/probe builds and runs `20260907T013843-a5cf04e4` (29854870)
and `20260907T013844-4eb30921` (KB2000) each complete **52 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and draw capture evidence. Both TLS models pass all
three cycles on both devices.

Limits: the static resolver now saves registers and invokes the existing
initializer (including its registry mutex) on each access. Performance and
signal-handler/reentrant access are not validated. IE TLS accesses that do
not call this resolver still require an initialized thread. This is not proof
of every vendor TLS destructor, compat heap cleanup, DSO unmapping or static
slot reclamation. Those remaining cases must be tested separately.

## TLS registration catch-up across threads

The isolated `tls-bounds` process also registers an initialized byte on main,
then a different byte on a fresh worker. The worker must see both initializers;
main must catch up with the second initializer while preserving a local mutation
of the first. No Android library or GPU object is loaded by this case, so its
synthetic offsets cannot overlap vendor TLS.

Before the fix, `20260907T014423-1a5cb51e` read worker values `0,29` instead
of `17,29`: registration advanced its per-thread cursor past modules registered
on other threads without replaying them. Registration now applies its missing
entries under the existing mutex before advancing that cursor. Previously
initialized entries are left intact.

Fresh library/probe builds and runs `20260907T014600-82060cd9` (29854870) and
`20260907T014600-448b50be` (KB2000) each complete **52 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Both workers read `17,29` and both
main threads preserve the mutated first value `41` while receiving `29`.
All 130 common dynamic exports remain unchanged. This directly verifies the
registry callback's replay bookkeeping, not concurrent ELF loading, static slot
reclamation or signal-safe initialization.

## Foreign-thread MRS first touch

Foreign-thread MRS first touch (2026-09-07): AArch64 TLS thunks now check
bionic TLS slot 1 before returning the adjusted thread pointer. On first
access a separate assembly helper preserves integer/NEON registers, NZCV,
FPCR and FPSR while initializing the compatibility TLS; the C helper restores
host errno. The bionic pthread shadow's tid prefix receives SYS_gettid.
Initialized-thread reads do not call the helper. Q linker reservations and
patcher allocations share the 80-byte thunk size; MRS to XZR is left intact.

The standalone `tls-mrs` probe enters a naked bionic fixture on eight fresh
glibc threads, before any bionic libc/TLSDESC access. It checks selected
caller-saved integer/NEON state, NZCV, errno and the shadow tid over 32 reads
per thread. Old-library negative control `20260907T062749-34d32fa1` on X300
fails all eight workers with observed=-1; the new library passes on both
phones. This does not exhaust all destination registers or prove signal,
fork, cancellation/unwind, SVE/SME, allocation-failure or arbitrary Android
pthread layouts. Modules promoted after initial thread setup still require
a hook/TLSDESC resolver to replay their initializers; the fast MRS path does
not catch up such modules.

Full post-fix Redmi run `20260907T062705-eb2706e7`: **96 PASS,
4 UNSUPPORTED, 1 CRASH** (native-groups). X300 run
`20260907T062706-9fb06554`: **83 PASS, 4 UNSUPPORTED, 13 CRASH, 1 FAIL**.
Frontend vk-init/TLS and ICD vk-init now pass. X300 retains native-groups,
five ICD widget variants, five widget validation variants and two capture
crashes, plus ICD core11 transfer failure. These remain open; this batch
only resolves the observed foreign-thread first-touch failure. No unit-test
suite was added.
