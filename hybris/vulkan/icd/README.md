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
No dispatch header, pNext list or extension list is rewritten. Instance proc
queries retain HAL scope/enable checks; GIPA and destruction stay in the
adapter. Device/resource state and the replacement-libvulkan frontend are
not covered by this table.

`HYBRIS_ICD_INSTANCE_TRACE=1` emits create/destroy generation and raw HAL handle
records to stderr. It is ignored in secure execution, disabled by default and
capped at 256 records plus a truncation marker. The destroy event denotes
removal from the adapter table before backend destruction, not GPU completion.
`icd-vk-init` checks the 16 matched lifetimes from its four workers; this is not
an arbitrary application resource trace or proof of full driver unloading.

Interface version 5 is required. Instance version discovery uses the HAL query
when available and otherwise Vulkan's 1.0 fallback. The physical-device resolver
uses an exact registry-derived scope table, including aliases. That table is
not proof of complete frontend export/dispatch coverage. It does not advertise
functions absent from the HAL.

Android normally owns surface/swapchain behavior in its loader. This adapter
does not implement WSI and rejects HALs advertising driver-owned KHR_surface
or KHR_display instead of passing incompatible surfaces through. Windowed
application compatibility, full validation and capture/replay remain open.

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
