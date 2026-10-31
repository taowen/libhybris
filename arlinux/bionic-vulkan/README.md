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

The WSI queries Android hardware-buffer import support before advertising
8-bit RGBA/BGRA or 10-bit RGBA surfaces. Native buffers retain the chosen
channel order and precision. Opaque RGBX backing is available only when the
reported format set supports that format-independent guarantee. The Android
compositor uses OpenGL ES/EGL.

Build and validation procedures are in the [component guide](../BUILD.md).
