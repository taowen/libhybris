# GLES context probes

Context isolation, sharing, migration and concurrent drawing. These are GLES workloads, not desktop GL validation; see [desktop GL](../desktop-gl/README.md) for that frontend.


## Queried stage limits

GLES 3 probes also report per-stage SSBO/image limits and vertex sampler/UBO
limits when the actual context supports GLES 3.1. Each `GLES_LIMIT` includes its
GL error and an invalid query fails the probe. A GLES 3.0 context reports the
stage-limit queries as unsupported while retaining its ordinary drawing check.
Compare native and hybris values on the same device; reported limits alone do
not establish desktop GL or application compatibility.

## EGL context isolation and migration

`egl-life` creates two unshared GLES2 contexts and two RGBA8 pbuffers. It
checks that the second context initially cannot see the first context's
buffer, then retains distinct buffer sizes/bindings and red/green clear state.
Eight switch pairs on the main thread, eight on a worker and eight back on the
main thread verify current context/read/draw surfaces and exact RGBA pixels.
The main thread releases current before pthread_create; the worker unbinds and
calls eglReleaseThread before pthread_join returns. Resources are deleted and
contexts/surfaces destroyed, then the sequence repeats for three cycles.

Runs `20260907T011354-4882a8fc` (29854870) and `20260907T011355-938bfc04` (KB2000)
each completed **47 PASS / 2 UNSUPPORTED**, including native and hybris
`egl-life`; optional Vulkan validation/capture were not selected. This probe
requests GLES2 and checks buffer objects, clear state and pbuffer pixels. It
does not prove context sharing, simultaneous rendering, shader state migration,
actual Android TLS destructor execution, handle generation management, FD
leak freedom or full GLES2 conformance. Failure paths terminate the isolated
probe process rather than attempting recovery of a failed EGL context.

## GLES share-group lifetime

`egl-life` now follows each isolated-context cycle with two GLES2 contexts in
one share group. The first creates a 16-byte buffer and a one-pixel RGBA texture
containing red. The second sees both objects, starts with array-buffer binding
zero, reads red through its own FBO, resizes the buffer to 32 bytes and uploads
green. The first sees the new size and green pixel, then is destroyed. The
second must still see the 32-byte buffer and exact green pixel before deleting
the shared objects and its context. The sequence repeats three times.

Each handoff uses glFinish and explicit resource rebinding/attachment. This
checks sequential shared-object visibility and survival of creator-context
destruction; it does not test simultaneous rendering, cross-context fences,
shared-object deletion while another context references it, buffer contents,
shader/program sharing or general object generation tracking.

Fresh probe builds and runs `20260907T023959-ac41857d` (29854870) and
`20260907T023959-961f4ab9` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native and hybris each pass all three
shared-context cycles on both devices. Production code is unchanged.

## Simultaneous current contexts on separate threads

The isolated half of `egl-life` now includes two workers, each owning one
context and pbuffer. Both attempt to make their context current before main
opens the start gate. Each then performs eight state checks and clear/readback
iterations, requiring the original buffer binding/size and exact red or green
pixel. No mutex serializes their GL calls. Each releases thread state before
join, and main switches between both contexts to check preserved state again.
The sequence repeats for all three lifecycle cycles. Partial thread creation
releases the cancellation gate and joins all started workers.

This proves simultaneous current contexts and successful independent threaded
clear/readback. It does not establish GPU execution overlap, shader-draw
concurrency, shared-resource synchronization, application callbacks or general
generation tracking. Shared-context work remains sequential with glFinish.

Fresh probe builds and runs `20260907T024324-39d298a2` (29854870) and
`20260907T024324-8d0ade38` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native and hybris each report six
successful worker results across three cycles on each device. Production code
is unchanged in this batch.

## Shader draws during context migration and concurrent use

The isolated contexts in `egl-life` now each retain a linked program and vertex
buffer for a full-screen triangle. Context 0 writes blue and context 1 yellow,
distinct from their red/green clear colors. Every existing iteration checks
GL_CURRENT_PROGRAM, draws without rebinding the program or vertex attributes,
and requires an exact center pixel and no GL error. This runs on main, through
single-worker migration, on two independently current worker contexts, then
again on main. Programs and buffers are deleted before context teardown.

This adds concurrent host-thread shader submission and program/vertex state
isolation to the previous clear/readback evidence. Programs are compiled on
main before migration; it does not test concurrent compilation, shared shader
objects, synchronization between shared resources, general GLSL compatibility
or overlapping GPU execution.

Fresh probe builds and runs `20260907T024950-483553de` (29854870) and
`20260907T024950-5322ce95` (KB2000) each complete **60 PASS / 2 UNSUPPORTED**,
including VVL, SyncVal and capture/replay. Native and hybris both pass the
expanded EGL lifecycle workload on each device. Production code is unchanged.
