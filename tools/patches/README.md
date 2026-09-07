# Build-time tool patches

`spirv-tools-flatten-debug-iterator.patch` applies to SPIRV-Tools
`f289d047f49fb60488301ec62bafab85573668cc`, the dependency pinned by Vulkan SDK
1.4.309.0. `FlattenDecorationPass` walks the debug instruction list after
expanding groups. Its iterator advances for `OpName` but not for other entries
such as `OpMemberName`, so a valid grouped module with member names can hang.
The patch adds the missing advance; it does not remove decorations, validation
checks or application debug information.

The matching VVL invokes this pass while recording shader module creation.
The grouped baseline fixtures reproduce the hang with the pinned Debian VVL;
the patched layer is built from exact source commits by
`../build-validation-layer.sh`. Its manifest identifies the toolchain, source
revisions, patch and resulting ELF. Supply that manifest to baseline's
`--validation-build-manifest` to associate a run with the actual layer build.
The prebuilt archive returned by `../fetch-validation-layer.sh` remains
unchanged for comparison and is affected by this grouped-module issue.

Sources: [pinned SPIRV-Tools pass](https://github.com/KhronosGroup/SPIRV-Tools/blob/f289d047f49fb60488301ec62bafab85573668cc/source/opt/flatten_decoration_pass.cpp),
[pinned VVL call site](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/3ba0e590a2e76475b3eef4043355b498fb64e752/layers/state_tracker/state_tracker.cpp).
