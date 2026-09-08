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


`gfxreconstruct-empty-submit.patch` applies to GFXReconstruct
`c2ff0eecc7a7f43aa236a5c98097a685b928b782`. Its resource-dump submit loop
previously issued work only when its command-buffer vector was nonempty. An
original empty submit can still carry semaphore waits/signals or the final
fence. The patch issues those original submits as well; it does not remove a
wait, synthesize a signal, or change the application's command-buffer work.
The desktop Zink fixture reproduced a missing timeline signal and timeout;
with the patch, the same capture exports all 27 readbacks and its 15 saved
images match. [Desktop evidence and limits](../../tests/desktop-gl/capture.md)
include the default descriptor-buffer failure and Turnip pixel mismatch.

`../build-capture-tools.sh` retains the fixed upstream revision, applies this
exact patch and rejects any other tracked/staged source changes. It accepts
that exact patch on repeat builds, records the actual source-tree hash,
compiler, patch, recipe, build script and CMake cache, and verifies installed
file hashes when staging tools. The patch does not implement general
cross-queue wait-before-signal replay or repair the separate descriptor-buffer
capture/replay behavior.

Source: [pinned resource-dump submission loop](https://github.com/LunarG/gfxreconstruct/blob/c2ff0eecc7a7f43aa236a5c98097a685b928b782/framework/decode/vulkan_replay_dump_resources.cpp).
