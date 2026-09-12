# Preserve Blender shader interfaces, 2026-09-09

Automatic ClipDistance preparation no longer removes unused interface members
from individual shader modules. This fixes the Mali Blender startup error
`VUID-RuntimeSpirv-OpVariable-08746` without moving validation ahead of the
compatibility layer or suppressing the error.

## Cause and change

The original failing post-compatibility run, `20260909T195324-9c5ecb6a`, reports
vertex output `{Position, PointSize}` against geometry input
`{Position, PointSize, ClipDistance, CullDistance}`. The before-compatibility
capture `20260909T195941-73551413` contains matching four-member blocks. The
geometry shader accesses Position through an array of per-vertex input blocks;
it does not access the clip/cull members. Both original modules pass independent
SPIR-V validation without ClipDistance/CullDistance capabilities.

Module-local declaration removal cannot know the interface required by the
adjacent stage. Automatic preparation now preserves all declarations, types,
access chains and decorations. It removes only ClipDistance/CullDistance
capabilities whose corresponding input and output uses are proven inactive.
The existing conservative analysis now understands the leading vertex index
of an array of interface blocks. Active members, whole-block accesses, aliases,
unknown references, unsupported annotations and transform-feedback forms retain
their capabilities. Caller memory and allocation ownership are unchanged.

The explicit `HYBRIS_VULKAN_COMPAT_UNUSED_BUILTINS` experiment remains a separate,
output-declaration cleanup option. It is not enabled by the Blender launcher or
by the automatic ClipDistance path. Pipeline-scoped PointSize cleanup retains its
existing behavior. This change does not claim general active clipping across
geometry/tessellation stages or whole-application acceptance.

## Evidence

The final AArch64 build succeeded (`/tmp/libhybris-interfaces-build-final.log`).
No unit test was added. Existing device probes and actual captured shader modules
provide the following checks:

| Check | Result |
| --- | --- |
| 369 captured Blender startup modules through the new preparation function | All byte-identical; all pass `spirv-val --target-env vulkan1.2` |
| Existing active clip vertex/fragment and unused-builtins fixtures | Byte-identical in automatic preparation; independently validate |
| Mali existing core-feature, PointSize VVL and forced scaled builtins VVL probes | PASS `20260909T215522-62877954` |
| Turnip same existing probes | PASS `20260909T215522-6b6073f2` |
| Mali Blender, VVL after compatibility | OBSERVED `20260909T215607-7621e2c7`, zero errors/warnings |
| Turnip Blender, VVL after compatibility | OBSERVED `20260909T215651-e223bb44`, zero errors/warnings |
| Mali capture after compatibility | `20260909T215652-498ae971`, nine presents; affected VS/GS/FS byte-identical to original capture |

The captured startup modules contain no removable clip/cull capabilities, so
their unchanged-byte check does not exercise the capability-removal branch.
The active clip fixture checks retention, not hardware clipping pixels. Existing
PointSize and explicit builtins probes retain their pixel, declaration/access
remap and independent shader validation checks. The startup screenshots show
Blender's viewport and UI; they do not prove editing/saving/reopening.

An initial new run (`20260909T215159-f989e9e2`) stopped before Vulkan because
`libtbb.so.12` was absent. Both devices then installed Debian Blender
`4.3.2+dfsg-2` and its dependencies through the product apt/runtime path. Successful
observations still use the separately staged acquire-recreation-fixed executable
(`83916197…`), not that unpatched apt executable.

The layer was packaged into both GPU overlays, built into APK
`1b4105cc4a73e5ffe0987caf2895fdac61bfde23636126f74b84977e4fed25bd`,
installed and started on both devices. Both packaged and installed layer files
match `ed268fbbf19e32c0c282ab71568017e6d72cf45a240fb86e76f305015dc2a5d9`.
Runtime hashes and default/explicit layer environments also match. Product GLX
and Wayland teapot present/resize pixel checks pass after installation:
Mali `20260909T220113-718c9b96`, Turnip `20260909T220113-b479328e`.
These product teapot checks are distinct from the staged Blender observations.

Local records are in `/tmp/mali-blender-interface/`,
`/tmp/libhybris-unified-app-results/`, `/tmp/libhybris-features-results/`, and
Arlinux `build/blender-vulkan/shader-interfaces/`. The latter retains the three
before/after pipeline modules, inspection reports, app logs/screenshots and
installed-product checks. The earlier failing run remains as the control.
Blender's acquire fix packaging, full workflows, capture replay pixels and the
remaining gaps are still unfinished. Blender through Zink is not required.


## Application workflow follow-up

Subsequent unified-layer runs `20260909T221555-8fd1ebb4` (Turnip) and
`20260909T221618-99521293` (Mali) completed scripted model/save/fullscreen/restore/
reopen operations, with zero VVL error markers and inspected final model
screenshots. See Arlinux's [workflow evidence](../../../tests/blender/README.md)
for exact scope, the retained initial script failure and remaining product
installation boundary. This does not add shader corpus or replay coverage.
