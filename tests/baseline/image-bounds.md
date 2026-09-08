# Image-bounds Select rewrite

`HYBRIS_VULKAN_COMPAT_IMAGE_BOUNDS=1` enables an opt-in ICD pass at shader
module creation (and on pipeline-rebuilt modules). After `OpSampledImage` or
`OpImageFetch`, if the next `OpSelect`'s false object is a zero constant
(scalar or all-zero composite), that operand is replaced with the true object.

This matches Vortek's Mali+DXVK `removeImageBoundCheck` operand swap. It
changes out-of-range zeros to the in-bounds value. The switch is off by default
and is not auto-gated on Mali or DXVK. An intervening `OpStore` still clears
the watch, as in Vortek.

Host check: assemble `shaders/image-bounds.spvasm`, compile
`image_bounds_check.c` with `hybris/vulkan/compat/spirv_image_bounds.c`, and
`spirv-val` the rewritten module.
