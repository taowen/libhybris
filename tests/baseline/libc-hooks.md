# Bionic libc hook probes

Stream semantics and hook-table publication, with original failure and regression records. Code-review fixes are distinguished from independently reproduced failures.

## Stdio implementation split

Stdio module split: `bionic_stdio.c` owns 78 compiled FILE hooks, standard
stream mapping/storage, wide streams, mount-table streams and buffer queries.
The central registration stays in `hooks.c`, which drops 695 lines. Moved
function bodies and hook registrations were compared with the prior revision;
the existing public endmntent hook remains public and new cross-file symbols
remain hidden. Defined export sets remain common=130, Vulkan=643, ICD=3.
This is structural regression coverage using the existing workloads, not
exhaustive stdio semantics: 32-bit FILE/offset ABI, all wide/mount-table
operations, error states and concurrent stream access remain unverified.
Rebuilt runs `20260907T034933-8421f3d3` (29854870) and
`20260907T034933-d7d8d7cd` (KB2000) each completed **69 PASS / 2 UNSUPPORTED**,
including allocation callbacks, synchronization, TLS, validation/SyncVal and
capture/replay. No new API capability is claimed by the file split.

## Stream flushing

`stdio` runs the same bionic fixture natively and through Android DSO imports.
It writes buffered file content, calls fflush(NULL), and checks bytes via
pread before closing; it also flushes an open_memstream and checks its
published pointer, length and contents before closing. Its file lives in the
runner's isolated directory and is removed on success. The shared source is
part of the probe manifest. The old common library crashed with exit 139 in
`20260907T035222-4e6ec2cc`, while native passed. The memory case was not reached
in that old hybris process and has no separate captured pre-fix failure.
The bridge removes the descriptor precheck from fflush and fflush_unlocked:
NULL must reach glibc's flush-all operation, and descriptorless memory streams
must still flush. The unlocked entry is not independently exercised here;
concurrent flushing, input streams and wide-stream cases remain unverified.
Rebuilt runs `20260907T035349-1a7cf3cc` (29854870) and
`20260907T035349-ede7da43` (KB2000) each completed **71 PASS / 2 UNSUPPORTED**,
including both stdio cases in native and hybris, validation/SyncVal and
capture/replay. Defined exports remain common=130, Vulkan=643, ICD=3.

## Stream positions

The stdio workload also covers normal/64-bit fgetpos/fsetpos: save offset 3,
read one byte, restore and reread the same byte. Pipe queries require -1,
ESPIPE and bionic's position=-1 result. In old run `20260907T035705-cab681fa`,
native passed both sizes while hybris wrote position=549201271456 on the
ordinary pipe error; its 64-bit case was not reached. The bridge now uses
ftello/fseeko and their 64-bit variants, matching bionic's offset-only model
and avoiding reads of uninitialized/private glibc fpos fields. See the
[bionic implementation](https://fuchsia.googlesource.com/third_party/android.googlesource.com/platform/bionic/+/01e7576d8ba146a73fe9b1c3eb67130c471e06f0/libc/stdio/stdio.cpp).
These are AArch64 byte-stream cases; 32-bit overflow, multibyte conversion
state and large-file boundary cases remain unverified.
Rebuilt runs `20260907T035849-5ba9e5ab` (29854870) and
`20260907T035849-1dcc8353` (KB2000) each completed **71 PASS / 2 UNSUPPORTED**,
including ordinary/64-bit position cases, validation/SyncVal and capture/replay.
Both hybris pipe queries now return -1, ESPIPE (29), position=-1, matching
native. Defined exports remain common=130, Vulkan=643, ICD=3.

## Hook table publication

Hook registry sorting now uses pthread_once so all sorted tables are
published before bsearch; the custom callback still executes first. The
old static sorted flag permitted concurrent qsort/search by direct callers.
The internal lookup is not exported, so the initial direct-dlsym probe could
not execute and was removed (runs `20260907T050526-1ac28fa4` and
`20260907T050526-44a8db69` reported that probe FAIL). No public export was
added for testing. Existing concurrent linker init and GPU lifecycle cases
provide regression coverage, but do not independently prove first-sort
contention: the linker itself has serialized initialization. This fix is
based on code review; no repeatable old sorting failure is claimed. Concurrent
callback replacement and mutable diagnostics remain outside this change.

Final rebuilt runs `20260907T050734-e0b8a4ba` (29854870) and
`20260907T050734-8e590b0e` (KB2000) each report 92 PASS, 2 UNSUPPORTED,
including concurrent initialization, VVL/SyncVal and both capture gates.
Common's 130 defined dynamic exports are unchanged.

## Hook callback and diagnostic publication

Hook callback publication uses an atomic pointer with one snapshot per query;
missing-symbol diagnostic IDs use atomic decrement and the optional unhooked
logging flag uses pthread_once. These are code-review fixes, with existing
concurrent initialization and GPU runs used for regression. The hidden lookup
prevents the direct standalone lookup probe used in the earlier attempt; no
isolated callback-replacement or missing-symbol race reproduction is claimed.
The setter does not synchronize the lifetime of a callback already selected
by another thread; this is now documented in the public header. Concurrent
environment mutation, SDK configuration and trace internals remain outside
this batch.

Rebuilt runs `20260907T051103-85f6e8df` (29854870) and
`20260907T051103-d3f11c87` (KB2000) each report 92 PASS, 2 UNSUPPORTED,
including concurrent init/lifecycle, VVL/SyncVal and both capture gates.
Common's 130 defined dynamic exports remain unchanged.
