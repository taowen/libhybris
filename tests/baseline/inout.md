# Vertex/fragment Location width matching

`HYBRIS_VULKAN_COMPAT_INOUT=1` enables an opt-in ICD rewrite at graphics
pipeline creation. When a vertex output and fragment input share a Location
and the vertex vector is wider, the fragment input is widened and loads keep
the original prefix via `OpVectorShuffle` / `OpCompositeExtract`.

This is the Vortek DXVK `checkInOutVariablesSize` algorithm with valid SPIR-V
load types. It does not copy Vortek's pointer-only patch, image-bounds
`OpSelect` rewrite, command IPC, or clip-distance deletion.

Unsupported forms (structures, BuiltIn/Component decorations, extra uses of the
input variable) leave the original fragment module. The switch is off by default
and ignored for secure processes.

Host check: `python3 tests/baseline/shaders/generate-inout.py` then compile
`inout_rewrite_check.c` with `hybris/vulkan/compat/spirv_inout.c` and run it
against `spirv-val`. Covers vec2 and scalar fragment inputs against a vec4
vertex output, and a matching vec4/vec4 negative.
