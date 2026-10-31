# Bionic system Vulkan

The Wine graphics path is:

    DXVK -> Wine Vulkan -> desktop Vulkan loader
         -> VK_LAYER_HYBRIS_compat -> desktop ICD/WSI -> Android Vulkan HAL

The ICD/WSI and compatibility layer come from the maintained libhybris source.
They are separate libraries. The ICD supplies desktop surfaces and transports
Android native buffers; the layer converts unsupported features, shaders and
resources together. Porting only the ICD cannot satisfy DXVK on Mali.

`native.cpp` supplies Bionic loader hooks and an AHardwareBuffer allocator.
It replaces the foreign-libc bridge and legacy gralloc allocation, while
preserving the shared WSI's buffer ownership and presentation implementation.

`tools/build/bionic-vulkan.py` stages the shared sources, compiles both libraries
with the Android NDK and checks undefined symbols. It takes the compatibility
layer's source list from libhybris's Makefile so its related transformations
stay together. The runtime manifest enables the layer explicitly; its
capability policies select missing features, rather than forcing conversions
on drivers that already implement them.

The WSI exposes RGBA surface formats so opaque swapchains can use native RGBX
buffers. The Android compositor uses OpenGL ES/EGL.

See `docs/WINDOWS-BIONIC.md` for build instructions and device validation,
including the remaining Mali WoW64 host-memory restriction.
