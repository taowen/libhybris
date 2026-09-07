# Standard-loader adapter

This optional library connects the glibc Khronos Vulkan loader directly to an
Android Vulkan HAL through libhybris. It does not load Android libvulkan, modify
dispatchable object headers, build a private layer chain or edit pNext lists.
HAL objects already reserve the loader dispatch word; the glibc loader owns it.

The frontend libvulkan remains the default delivery path. No system ICD manifest
is installed. Baseline run.py can stage a private manifest with --icd-hal and
--vulkan-loader. The HAL override is ignored for secure execution; without an
override the hardware module lookup selects the Vulkan HAL. The adapter and
HAL remain resident. Full driver unloading is not implemented.

The adapter owns an instance record containing its real HAL resolver,
destruction entry and a process-lifetime unique generation. Records are
allocated before HAL creation, published only on success, removed at destroy
and freed afterward; callbacks and HAL calls run outside the list lock.
Application allocation callbacks also cover the record when supplied.
No dispatch header or pNext list is rewritten. Local platform WSI names are
appended on instance extension enumeration and stripped from the HAL
CreateInstance list; other enabled names and the original pNext chain are
left unchanged. Instance proc queries retain HAL scope/enable checks, then
return those local surface entry points when the instance enabled them. GIPA
and destruction stay in the adapter. The resolver still does not scan the ICD
ELF export table. Device/resource state beyond this local swapchain and the replacement
libvulkan frontend are not covered by this table.

`HYBRIS_ICD_INSTANCE_TRACE=1` emits create/destroy generation and raw HAL handle
records to stderr. It is ignored in secure execution, disabled by default and
capped at 256 records plus a truncation marker. The destroy event denotes
removal from the adapter table before backend destruction, not GPU completion.
`icd-vk-init` checks the 16 matched lifetimes from its four workers; this is not
an arbitrary application resource trace or proof of full driver unloading.

Interface version 5 is required. Instance version discovery uses the HAL query
when available and otherwise Vulkan's 1.0 fallback. The physical-device resolver
uses an exact registry-derived scope table, including aliases. That table is
not proof of complete frontend export/dispatch coverage. Apart from adapter-owned WSI commands, ordinary command lookup retains the
HAL resolver and its scope/alias availability.

Android normally owns surface/swapchain behavior in its loader. This adapter
still rejects HALs advertising driver-owned `VK_KHR_surface` or
`VK_KHR_display` rather than passing incompatible surfaces through. When
built with Wayland, it advertises `VK_KHR_surface` and
`VK_KHR_wayland_surface` itself, creates local `VkSurfaceKHR` objects with
the existing `window_owner` native-window factory. When the HAL advertises
`VK_ANDROID_native_buffer` revision 8 or later, a graphics queue and usable
color-attachment formats, the adapter exposes an experimental FIFO swapchain.
Native buffers are retained until their imported images are destroyed, including
acquired images of retired swapchains. A failed replacement also retires its
old chain. Dequeue uses Wayland read preparation and a monotonic poll timeout.
A multi-swapchain present consumes application waits once and signals an
internal semaphore for each image, which its Android release operation waits
on before the native window receives the buffer. Presentation still performs a
host wait on release FDs because android_wlegl has no per-commit fence protocol;
there is no queue/device wait-idle in this implementation.

Format, usage and extent queries use HAL image-format creation queries; image
count 2–8, one layer, identity transform, inherited native alpha and FIFO are
adapter constraints. Single-device group queries and AcquireNextImage2 are
implemented. Protected/multi-device modes and swapchain-backed image alias
creation/binding remain unsupported; alias handles are explicitly rejected
rather than forwarded to a HAL that cannot interpret them. This is not full
Vulkan 1.1 swapchain conformance. Missing android_wlegl maps to VK_ERROR_UNKNOWN.
The WSI runner can enable the standard Khronos validation layer and
GFXReconstruct on this window path; see tests/wsi/README.md. OnePlus 8T and
Mali have device evidence for create/render/resize/retirement/destroy under
VVL+SyncVal, and for capture/replay of the three-size window copies via
`--swapchain virtual`. Swapchain-backed image alias creation/binding remains
unsupported; this replay path did not require it. See the
[swapchain review and device evidence](../../../tests/wsi/swapchain-review.md) and
[validation/capture review](../../../tests/wsi/validation-capture-review.md).
The pinned tools require separate validation and capture runs; capture also
excludes the allocation-failure boundary workload.

## Experimental X11 WSI

With both Wayland and X11 enabled at build time, the adapter also exposes
`VK_KHR_xcb_surface` and `VK_KHR_xlib_surface`. It uses TAWC-DRI 0.3 over
local Unix sockets to submit gralloc handles, retaining buffers until actual
BufferRelease events. Missing protocol and incompatible visuals are rejected.
Xlib shares its XCB connection without changing application event ownership.
Current extent comes from X geometry; dynamic resize/out-of-date handling is
not complete. This is a fixed-size experimental path, not full X11 conformance.

The [independent X11 developer tool](../../../tests/x11/README.md) builds a
private protocol-enabled Xwayland and runs one selected check in the disposable
compositor APK. Both Adreno and Mali passed XCB/Xlib present, protocol rejection
and acquire timeout with VVL/SyncVal. No Mesa or anlabwc change was required.
See that document for protocol provenance, exact runs and remaining G11 scope.

## Sources

hwvulkan.h is copied unmodified from the Android Open Source Project:
https://android.googlesource.com/platform/frameworks/native/+/7b6a56ef2a/vulkan/include/hardware/hwvulkan.h
SHA256: 5f16023c9edb816f2387b5c890c17dca3df768a8bc0cb74bc3ed263418aa005a.
Its Apache-2.0 copyright/license notice is preserved.

physical_commands.inc is generated from Vulkan-Headers v1.4.309, commit
952f776f6573aafbb62ea717d871cd1d6816c387. Download registry/vk.xml from that
commit and run:

    python3 tools/registry/generate-physical-commands.py /path/to/vk.xml

The generator verifies the registry SHA256 before producing the table.
Loader contract:
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md

## Inspected Mali MMUD workaround (opt-in)

For the X300 Mali driver with GNU build-id
`5ac4efe8d6175298b273dbaeb8f9d28e5e508e72`, set
`HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK=1` before loading the HAL. The baseline
runner offers `--icd-mali-loader-quirk`, applied only to ICD commands and their
capture/replay commands. It is off by default, ignored during secure execution,
and requires an AArch64 build with Mali quirks. The common hook matches the
requesting `libGLES_mali.so` file's build-id; application hook callbacks still
have precedence. Other drivers and property names use the existing path.

This driver reads Android loader-private data from the instance header during
MMUD setup (observed crash at file offset 0xa237bc, header=0x1cdc0de).
Inspection of its decoder at 0x1db11d0 shows a process property encoding
`N*1000000+322126`; bit 3 of N bypasses that inspection. The hook reads the
original libcutils result and adds only that control bit for recognized
encodings (or default/nonpositive values); unknown positive encodings are
left unchanged with a diagnostic. Default 0 becomes 8322126. No Android system
property is written. The `HYBRIS_MALI_MMUD` log records the exact build-id and
before/after value; run metadata retains these observations.

The driver path also inspects layer presence and dispatch function addresses.
This is not a general implementation of Android loader-private data. Effects
on other MMUD behavior, performance, arbitrary layers and applications remain
unverified. The explicit opt-in is retained for that reason. Do not extrapolate
to another firmware or use this option to claim full Mali compatibility.
No dispatch header, Vulkan creation chain or validation rule is changed.

Build and full runs on 2026-09-07: Redmi `20260907T065438-27c734a0` has
96 PASS / 4 UNSUPPORTED / 1 CRASH (native-groups); the option produces no MMUD
activation on Adreno. X300 `20260907T065437-096b67ab` has
93 PASS / 4 UNSUPPORTED / 2 CRASH / 2 FAIL. Ordinary/dynamic/large/staged widgets,
their validation/SyncVal paths and both capture/replay gates pass. Native groups,
ICD core11 and template/template-validation remain failures. Captures remain
headless, with no WSI/present coverage. All saved ICD caps/caps2 query values
match the pre-workaround run. Same-built-library opt-out run
`20260907T065418-7b030d21` reproduces the original pipeline crash.
Other build-id rejection, nonzero encoded properties, malformed ELF notes and
secure execution have code checks but no independent runtime fault injection.

## Manifest version discovery

The baseline now runs `icd-version` directly against the adapter before any
standard-loader case. The probe negotiates interface 5 and calls the adapter's
`vkEnumerateInstanceVersion`, which forwards to the HAL. Its validated result
becomes the private JSON manifest's `api_version` and `device.json`'s
`icd_api_version`. Even a selected ICD case includes this prerequisite.
Discovery failure aborts the dependent cases, preserving the failed probe's
output and summary. No fixed version is substituted after a failure.

The previous manifest's hard-coded 1.0 was incorrect: the Khronos loader
checks the JSON version before consulting vkEnumerateInstanceVersion. A 1.0
manifest causes it to pass API 1.0 to the driver even when the application asks
for 1.1. Negotiating loader interface 5 does not bypass that check. This caused
Mali core11 proc lookup failure and an absent template implementation beneath
the loader trampoline. Older passing Adreno tests do not prove the requested
instance version reached the HAL.
Reference: https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-api-version

The adapter reports 1.3.305 on X300 and 1.1.128 on Redmi. These are observed
instance versions, not a conformance or full feature-execution claim. With the
Mali option above, final X300 `20260907T070031-042f6354` and Redmi
`20260907T070032-2711e780` both have 97 PASS / 4 UNSUPPORTED / 1 CRASH
(native-groups). This includes core11 transfer, template drawing/validation,
ordinary/dynamic capture and the version probe. Missing-HAL negative control
`20260907T070107-1a7bcb32` reports icd-version FAIL and runs no dependent case.
The 1.0 HAL fallback and multiple HALs in one process remain untested here.

## Experimental scaled vertex fallback

`HYBRIS_VULKAN_COMPAT_SCALED_VERTEX=1` enables an initial scaled vertex fallback
in this standard ICD. It is disabled by default and ignored for secure
execution. For R/RG/RGBA 8-bit and 16-bit USCALED/SSCALED, a missing vertex-buffer
format is replaced with UINT/SINT only when that integer vertex format is
supported. FormatProperties and the core/KHR FormatProperties2 queries add
only the vertex-buffer bit; a supplied FormatProperties3 receives the matching
bit. Image capabilities and other format features are unchanged.

The adapter captures original shader code, clones affected graphics pipeline
inputs, changes vertex fetch to integers, and creates a temporary vertex
shader with integer-to-float conversions at the original loads. Signed minima
remain exact. Original result IDs, shader modules, specialization data and
unmodified pipeline state are retained. Temporary shaders and command-scoped
copies are freed after the backend pipeline call, including failure paths.
Application allocation callbacks cover device records, shader copies and
pipeline work. Module destruction follows Vulkan's host external
synchronization requirement:
https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyShaderModule.html
All helpers are hidden; the ICD still exports only its three loader entries.

This is an experimental subset, not a conformant implementation of arbitrary
scaled vertex pipelines. The rewriter selects the named vertex entry from a
module, including modules with multiple vertex or other-stage entries. The
temporary module exposes only that entry and its execution modes. A separate
decoration pass first expands group applications into direct variable/member
decorations. Group definitions, original annotations and debug names remain;
only the applications are replaced. This lets Location, member layout and
SpecId decorations reach the later passes without treating member indices as
IDs. Grouped string annotations and ID-operand decorations applied to members
remain unsupported. Legacy grouped OpDecorateId applications to variables are
implemented but are not independently GPU-tested.
An entry-extraction pass follows function calls and removes unreachable functions,
unreferenced global variables, and their names/decorations. Types and constants
are retained. Other multi-entry stages in an affected graphics pipeline also
receive temporary modules for their selected entries; the original module can
still supply later pipeline variants. All temporary stage modules are freed
after the backend call. Supported inputs are direct Location
scalar/vec2/vec3/vec4 float32 values, direct loads, component
access chains and pointer copies. Float32 matrices and fixed-size arrays
(including nested arrays and arrays of matrices) with a root Location are
lowered by a separate aggregate pass. Each leaf becomes an input at its own
Location; entry initialization reconstructs the original float aggregate in
Private storage. Only leaves with an active scaled attribute become integer
fetches, so native float columns can coexist with signed and unsigned scaled
columns. Dynamic access chains and whole-aggregate value loads preserve their
float types. SPIR-V 1.4+ entries retain the used Private global in the interface;
earlier versions list only the new Input leaves.
Array lengths may also use scalar 32-bit integer specialization constants and
supported integer/boolean expressions. A separate constant evaluator reads the
current stage's `VkSpecializationInfo`, including defaults, and the aggregate
pass freezes only the resolved array-length result IDs. It removes SpecId only
from constants that were frozen. The original specialization map still reaches
the driver for other uses, including float and boolean values; the original
shader module and the application's pipeline cache are retained. Supported
expression operations include integer arithmetic, comparisons, logical/bitwise
operations, shifts, Select and same-width integer conversions. Zero divisors,
invalid shifts and unsupported expression types fail conversion.
Interface blocks/member Locations, float/64-bit/composite expressions in array
lengths, affected float16/float64 aggregates and unhandled Input pointer forms
remain unsupported. Grammar-derived masks exclude definite literal operands
such as shuffle/extract indices and parameter-free enums from pointer/liveness
scans. Ambiguous or variable-width operand sequences still use conservative
scanning: a literal can cause rejection or retain an extra global declaration
when its position cannot be classified statically. Function-pointer instructions, unknown opcodes,
and unhandled nonsemantic debug references into removed code are rejected; this
is not a general SPIR-V optimizer. Unsupported
conversion returns `VK_ERROR_UNKNOWN`, with a diagnostic, rather than supplying
a partially rewritten module. With an active fallback mask, graphics pipeline
libraries and dynamic vertex input are rejected; shader objects, inline stage
modules and shader-stage extension chains are not supported by this fallback.
Do not enable it for applications requiring these paths. General SPIR-V
interfaces/control flow, extensions, general specialization/cache-key coverage and
full pipeline state coverage remain open. No clip/cull, point-size, BC texture or timeline emulation
is included. The replacement-libvulkan frontend does not apply this fallback.

For diagnosis, the value `force` converts these formats even when the vendor
supports scaled fetch, provided the integer format is supported. It is not the
normal workaround mode. `HYBRIS_VULKAN_SCALED_DUMP_DIR` optionally writes at most
128 original/converted module pairs into an existing directory, with exclusive
0600 files and per-pair location/signedness logs. It is disabled by default and
ignored for secure execution. The baseline runner's
`--scaled-vertex-compat missing|force` sets the option only for ICD cases and
collects these dumps for scaled probes; host `spirv-val` and `spirv-dis` are
required. See the baseline README for device results and reproduction.

The entry-extraction result-ID and definite-literal tables are generated from the Khronos SPIR-V 1.6
revision 7 grammar, SHA256
`db8581272b63d232268094a47b68d18a0464fc911e06004d57419924fe660ba4`, as vendored in
unmodified Mesa `c3b008c1ba01d455351b762253ef44c3ca19653f` at
`src/compiler/spirv/spirv.core.grammar.json`. Reproduce with
`python3 tools/registry/generate-spirv-results.py /path/to/spirv.core.grammar.json`.
The generator verifies the input hash. The checked-in table needs no Mesa or
SPIRV-Tools runtime dependency. The source grammar is available at:
https://gitlab.freedesktop.org/mesa/mesa/-/raw/c3b008c1ba01d455351b762253ef44c3ca19653f/src/compiler/spirv/spirv.core.grammar.json

`spirv_literals.inc` records fixed literal positions and all-literal suffixes.
The generator stops at ambiguous composites, strings, optional non-final
operands and parameterized enums (after classifying the enum word itself).
It does not guess the layout of 64-bit switch pairs or enum-dependent payloads.
Unclassified words retain the conservative behavior above. This is a bounded
operand classifier, not a complete SPIR-V validation grammar implementation.


For specialization diagnostics, the bounded scaled dump also stores each
supplied stage's raw specialization data (`NNN-specialization.bin`) and logs
constant-ID/offset/size mappings. Data over 64 KiB or more than 1024 map entries
is recorded as `saved=0`; the dump remains opt-in and limited to 128 modules.
The `scaled-vertex-spec` and `scaled-vertex-spec-direct` headless fixtures cover
expression/direct lengths, unrelated float/bool parameters, defaults, repeated
pipeline variants and same-process cache serialization/restoration. See the
baseline README for exact device results and remaining limits. Specialization
map behavior follows the [Vulkan specialization constants rules](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-specialization-constants).
