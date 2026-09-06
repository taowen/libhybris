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
