# Bionic synchronization timeout probes

Absolute, monotonic and relative wait behavior through real bionic imports. [Initialization and object ownership](sync-initialization.md) are separate workloads.

## Monotonic condition-variable aliases

The hybris-only `cond-clock` workload calls actual bionic imports of
pthread_cond_timedwait_monotonic and pthread_cond_timedwait_monotonic_np from
the fixture DSO. Each uses a fresh default-clock condition variable and a
CLOCK_MONOTONIC deadline 100 ms ahead, retaining that deadline across spurious
wakeups. It requires ETIMEDOUT and at least 90 ms elapsed, then unlocks and
destroys the objects. The existing process watchdog bounds excessive delays;
there is no tight upper timing assertion sensitive to host scheduling.

Before the fix, `20260907T025314-ab535531` on 29854870 returns ETIMEDOUT in
64,583 ns and 38,177 ns respectively: the aliases went to ordinary timedwait,
which treated monotonic timestamps as expired realtime deadlines. Both now use
the clockwait bridge with CLOCK_MONOTONIC explicitly. Fixture declarations
retain these legacy imports even where the current NDK hides their prototypes.

This verifies process-private absolute timeout behavior for the two aliases.
It does not validate wall-clock changes, relative waits, shared conditions,
cancellation, wakeup fairness or all condition-variable attribute combinations.

Fresh library/probe builds and runs `20260907T025449-9f0dad86` (29854870) and
`20260907T025449-72b2c6b8` (KB2000) each complete **61 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Both aliases return ETIMEDOUT after
about 100–102 ms on both devices. Common's 130 dynamic exports are unchanged.

## Relative condition wait validation

`cond-clock` also exercises pthread_cond_timedwait_relative_np through the
bionic fixture: 100 ms must end with ETIMEDOUT after at least 90 ms, while
nanoseconds equal to one billion, negative nanoseconds, negative seconds and
an unrepresentable LONG_MAX-second deadline must return EINVAL. Spurious
wakeups retry the relative duration; the no-signal workload does not assert a
tight upper timing bound. Conditions are explicitly initialized so invalid
inputs can be followed by legal destruction without requiring lazy allocation.

Before the fix, `20260907T025708-f2f78e33` on 29854870 incorrectly returns
ETIMEDOUT for one-billion nanoseconds after about one second, negative
nanoseconds after about 1.3 ms, and LONG_MAX seconds immediately. The wrapper
now validates the duration, uses checked addition for the absolute deadline,
and calls the common clockwait bridge with CLOCK_MONOTONIC. This removes the
duplicated translation path and dependence on a realtime absolute deadline.

No wall-clock jump is injected, and shared conditions, cancellation, wakeup
fairness and boundary arithmetic on 32-bit platforms remain unverified.

Fresh library/probe builds and runs `20260907T025846-dffa5ff2` (29854870) and
`20260907T025846-d4473ad5` (KB2000) each complete **61 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Relative 100 ms waits complete in
about 100–102 ms; all four invalid-duration cases return EINVAL (22). The 130
common dynamic exports remain unchanged.

## Legacy mutex timeout

The legacy `pthread_mutex_lock_timeout_np` hook now uses CLOCK_MONOTONIC
via pthread_mutex_clocklock and maps ETIMEDOUT to EBUSY, matching bionic's
legacy contract. The bionic DSO imports this symbol for a hybris-only check
inside `cond-clock`: acquire an unused static mutex with zero timeout, wait
100ms on the held normal mutex, require EBUSY and at least 90ms elapsed, then
unlock/reacquire/unlock/destroy. No wall-clock adjustment is performed.
The API is absent from LP64 native bionic, so this is not an AArch64 native
comparison or 32-bit ABI validation; shared mutexes also remain unverified.
Before the fix, `20260907T044806-0401322d` returned ETIMEDOUT=110 after
100664271ns instead of EBUSY=16.
Source: https://android.googlesource.com/platform/bionic/+/63860cb/libc/bionic/pthread_mutex.cpp

Rebuilt runs `20260907T044949-b035cb87` (29854870) and
`20260907T044949-b9115f54` (KB2000) each report 88 PASS, 2 UNSUPPORTED.
The legacy mutex probe now returns EBUSY=16 after 103955677ns / 100934062ns,
with acquisition/reuse/cleanup passing. Validation/SyncVal and both capture
evidence gates pass. Common's 130 defined dynamic exports remain unchanged.

## Monotonic mutex deadlines

`mutex-monotonic` covers the API-28 pthread_mutex_timedlock_monotonic_np
entry, newly registered in the hybris hook table. It shares mutex translation
with ordinary timedlock but supplies CLOCK_MONOTONIC to the host. Native
resolves the platform entry; hybris executes an actual bionic fixture import.
The probe obtains an unused static mutex, waits against a 100ms monotonic
absolute deadline while held, requires ETIMEDOUT and at least 90ms elapsed,
then unlocks and acquires again with the expired deadline and with a null
deadline before cleanup. Null deadlines route to ordinary blocking lock;
this probe checks the uncontended null case.
This does not cover PI/shared mutexes, wall-clock jumps or all timedlock
validation cases. The legacy millisecond API continues to require EBUSY.

Final rebuilt runs `20260907T045649-a2754b94` (29854870) and
`20260907T045649-f05cab0e` (KB2000) each report 90 PASS, 2 UNSUPPORTED,
including native/hybris monotonic mutex, legacy timeout, VVL/SyncVal and both
capture gates. Native/hybris deadlines expire after approximately 100ms and
return ETIMEDOUT=110; expired and null deadlines acquire the unlocked mutex.
The new hook is hidden; the common defined export set remains 130 entries.

## Monotonic rwlock deadlines

`rwlock-monotonic` exercises the newly hooked API-28 monotonic read/write
lock functions. Native resolves the platform entries; hybris calls actual
bionic fixture imports. The owner holds a writer while a worker attempts
read, then holds a reader while a worker attempts write. Each worker uses a
100ms CLOCK_MONOTONIC absolute deadline and must return ETIMEDOUT after at
least 90ms. After joining and releasing the owner lock, expired and null
deadlines must acquire the now-free lock and allow clean destruction. The
hooks reuse existing rwlock translation, use host clockrdlock/clockwrlock,
and route null deadlines to blocking rdlock/wrlock. This does not prove
fairness, shared-lock behavior, clock-step handling or contended null waits.

Rebuilt runs `20260907T050101-b55ee16e` (29854870) and
`20260907T050101-253dd56b` (KB2000) each report 92 PASS, 2 UNSUPPORTED.
Native/hybris read/write worker waits return ETIMEDOUT=110 after about
100–102ms, and expired/null unlocked acquisition succeeds. VVL/SyncVal
and both capture gates pass. Common's 130 defined dynamic exports match
the prior build; the new hook functions have hidden visibility.
