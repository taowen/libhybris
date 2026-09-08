# Unused clip/cull declaration cleanup (2026-09-08)

`HYBRIS_VULKAN_COMPAT_UNUSED_BUILTINS=1` enables an opt-in ICD shader cleanup.
It removes unaccessed direct output ClipDistance/CullDistance variables and
unaccessed members of ordinary output structures. It does not remove active
clipping, change physical-device features, or emulate clipping/culling on a
GPU that lacks them. PointSize is retained.

The parser and rewrite live in `compat/spirv_builtins.c`; the small ICD creation
adapter lives in `compat/shader_cleanup.c`. It preserves allocator ownership,
leaves the application module bytes unchanged and calls the device's recorded
backend resolver. GIPA/GDPA and loader-mediated ELF/linked entry paths share the
same handling. The switch is ignored for secure processes and is off by default.

For an affected structure, all instances and uses are examined conservatively.
A referenced member stays. A whole-structure access, pointer alias, initializer,
non-output pointer, nested use or an unrecognized reference prevents pruning of
that structure. Up to 64 members are analyzed; larger structures are retained.
For retained members, names/decorations and constant access-chain indices are
remapped together. New index constants leave shared constants unchanged. Empty
blocks are retained. A ClipDistance/CullDistance capability is removed only if
no corresponding declaration remains anywhere in the module.

Extension-bearing shader creation, SPIR-V extension declarations, grouped/ID
annotations and transform-feedback/geometry-stream capabilities retain the
original module. Unsupported forms are not guessed at. These are explicit
scope limits, not coverage claims for those forms.

When scaled conversion is also enabled, its module capture retains the original
application bytes. Cleanup runs after conversion and before the final shader
artifact is recorded, so source hashes and final SPIR-V verification still cover
the complete tested transformation. The native shader creation path also runs
cleanup independently of scaled format support.

## Evidence

The final AArch64 build completed and staged 17 runtime ELFs with 58 installed
ELF checks. The probe bundle built with NDK `27.3.13750724`. No unit-test
framework was added.

`shaders/generate-unused-builtins.py` derives an independently validated module
from the existing scaled rendering fixture. It moves the unused ClipDistance
and CullDistance members before Position and PointSize, forcing removal to
change the Position access index. It creates separate constants for this input
reordering. The existing pixel expectations, signed/unsigned fetch, pipeline
reuse and cache checks remain in place.

Offline compilation/inspection confirmed that the reordered module loses two
members and validates after rewriting. A module that actually writes both
clip/cull outputs remains byte-identical. These checks do not establish active
clip/cull rendering coverage on unsupported hardware.

Initial device runs `20260908T140931-b4dec438` (Redmi) and
`20260908T140931-a58b4cec` (Mali) remain recorded as FAIL: pixel checks and Vulkan
validation passed, but the old shader evidence rule rejected any change to a
retained structure's member decorations. That rule was incomplete for cleanup.

`builtin_evidence.py` now independently audits the actual SPIRV-Tools
before/after disassemblies: only clip/cull members may disappear, retained member
types/builtins must match, source accesses cannot target removed members,
retained access chains must use the correct new index, and shared constants
must remain unchanged. The reordered fixture must show both removals and a
verified access-chain remap. This precise check is used only when cleanup is
explicitly enabled; all existing pixel and other shader checks are retained.

Final runs `20260908T141339-d1ce8a85` (Redmi) and
`20260908T141339-8517cf9f` (Mali) pass GIPA, GDPA, ELF lookup and linked rendering
routes with cleanup plus forced scaled conversion. Each route tests all twelve
scaled formats. Multi-entry shader conversion and multi-UBO validation also
pass on both devices. Shader artifacts pass SPIR-V validation, original-source
hash checks, scalar/interface checks and the additional cleanup audit. Vulkan
validation reports zero errors.

This implements safe removal of a class of inactive declarations; it does not
establish a before/after application bug fix, general shader optimization,
PointSize suppression, active clip/cull emulation, or completion of G08.

Independent cleanup runs `20260908T141432-e60d8663` (Redmi) and
`20260908T141432-9a03d44f` (Mali), with scaled conversion disabled, pass multi-UBO,
caps and caps2. Cleanup logs confirm execution. All 342 Redmi and 480 Mali
`CAP_VALUE` records equal the preceding default-off BC control's values.

Cleanup-off controls `20260908T141605-06de40ef` (Redmi) and
`20260908T141605-9d5c16d2` (Mali), with scaled conversion still forced, pass the
reordered fixture using the original strict decoration-equality audit.
The recorded converted artifacts retain the inactive members in that mode.

An additional offline direct-output module validates before and after removal
of its single unused ClipDistance variable/capability (376 to 332 bytes). A
valid transform-feedback module containing the inactive reordered block remains
byte-identical. These are module-level checks, not device rendering coverage
of direct builtins or transform feedback.
