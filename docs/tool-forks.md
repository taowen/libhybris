# Maintained tool forks

Build-time patches have moved into maintained forks. The builders pin full
commit IDs and record repository URLs, source-tree hashes, toolchains and
installed-file hashes. To change either tool, commit and push the fix in its
fork, update the pin here, then rebuild and run the relevant device probes.

SPIRV-Tools' debug-iterator fix lives in
[taowen/SPIRV-Tools, branch ardesk-vvl-1.4.362](https://github.com/taowen/SPIRV-Tools/tree/ardesk-vvl-1.4.362),
commit `adc7d8b01ae855292822192ae870ab1df19e40a3`, based on upstream
`b40380bfa431d028fb7ca8eb375e4d21ea98a70e`. The earlier SDK 1.4.309-based
`94043c878ff46fc2d7d48084696ef5fb02f5e3ea` remains on branch `ardesk` for
reproducing old runs.
`FlattenDecorationPass` now advances past non-`OpName` debug entries, including
`OpMemberName`, instead of hanging on valid grouped modules. It preserves
decorations, validation checks and application debug information.

`tools/build-validation-layer.sh` builds VVL 1.4.362 with that dependency
commit and the matching official known-good headers/utility libraries. It
disables VVL auto-downloads (`UPDATE_DEPS=OFF`), so a CMake configure cannot
replace the selected fork or silently rebuild another set of dependencies.
The obsolete external robin-hood dependency has been removed; this VVL uses
its in-tree parallel hash maps. The prebuilt archive from `tools/fetch-validation-layer.sh` remains the
original upstream binary for comparison and is affected by this issue.


GFXReconstruct's empty-submit fix is maintained in
[taowen/gfxreconstruct, branch ardesk](https://github.com/taowen/gfxreconstruct/tree/ardesk),
commit `e6865bedad471a7ffaada05b6b18b29d77da36ee`, based on upstream
`c2ff0eecc7a7f43aa236a5c98097a685b928b782`. `tools/build-capture-tools.sh`
now pins `ab565b2792f98c6e0deebf182604f830d6deaed2`, which includes that
empty-submit repair and the subsequent capture/resource-dump fixes. It records
the repository, revision, source tree, compiler and installed file hashes. Future changes belong in the
fork, followed by a revision update here; no build-time patch is applied.

The resource dumper preserves original empty submissions' semaphore/fence
operations. This fixes the captured Zink timeline wait without inventing a
signal or changing command-buffer work. It does not implement general
cross-queue wait-before-signal replay or repair descriptor-buffer replay.
[Desktop evidence and limits](../tests/desktop-gl/capture.md) retain the
same-capture comparison and the separate Turnip pixel mismatch.


## Migration checks, 2026-09-08

Both AArch64 tool builds completed from the fork commits. Their manifests
record the fork URLs and revisions, with no `patch_sha256` field; the recorded
build-script hashes match the committed scripts.

- GFXReconstruct: Redmi `20260908T175746-dde3c283` has five PASS results
  (version, UBO and ordinary/dynamic/multiple-binding capture/replay).
- GFXReconstruct: Mali `20260908T175438-1b996178` passes basic desktop Zink
  rendering, capture and replay with lazy descriptors, the existing scoped
  loader quirk and `--replay-memory rebind`. Replay exports 13 readbacks;
  the saved main image matches exactly. Twelve packed-draw images are not
  saved by the probe and remain explicitly uncompared.
- Validation layer using the SPIRV-Tools fork: Redmi
  `20260908T181003-89fb74d0` and Mali `20260908T181003-ef0fb13b` each have
  five PASS results: version, group/group-multi/group-spec validation and UBO
  validation. Redmi uses scaled-vertex `missing`; Mali uses `force` and the
  existing scoped loader quirk. The grouped fixtures finish without hanging.

These are migration regressions, not additional coverage of the known
Mali descriptor-buffer or Turnip replay-image failures. Build outputs used
for the checks are `/tmp/libhybris-capture-empty-submit/install` and
`/tmp/libhybris-validation-fork/install`; device results preserve their
build manifests and exact tool/layer identities.


## VVL 1.4.362 regression, 2026-09-08

VVL is pinned to `538f91f14cd39274263eb15e6b4228f355370353`; the four
dependency pins follow its [official known-good list](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/538f91f14cd39274263eb15e6b4228f355370353/scripts/known_good.json),
with only the SPIRV-Tools debug-iterator repair added in our fork. All five
recorded input source snapshots remained byte-identical during the AArch64
build; the VVL build additionally created a Python bytecode cache, recorded
separately in `/tmp/libhybris-validation-current/input-verification.json`.
The builder uses the current in-tree hash-map dependency and explicitly disables
automatic downloads. The initial default-auto-download attempt was stopped
after it tried to fetch a second dependency set; it produced no accepted tool.
The final build script, source revisions, compiler, container and installed
file hashes are in `/tmp/libhybris-validation-current/install/manifest.json`.

Independent Redmi `20260908T204454-36b62dbe` and Mali
`20260908T204455-32fcbe6b` each have six PASS results: version, UBO validation,
three grouped-shader validation fixtures and the invalid-buffer control.
The latter still reports exactly `VUID-VkBufferCreateInfo-size-00912`, with
one expected error, zero other errors and `VK_ERROR_VALIDATION_FAILED_EXT`.
Grouped fixtures finish without hanging and retain their pixel/shader audits.
Redmi uses scaled `missing`, Mali `force`; no explicit MMUD environment switch
is needed by the current known-build common library.

The desktop Turnip run now passes independent VVL/SyncVal; the corresponding
Mali run still reports four real invalid direct draws. See the
[desktop comparison](../tests/desktop-gl/README.md#validation-dependency-upgrade-to-14362-2026-09-08).
These remain fixed headless regressions, not a shared product-window or
teapot/scene validation gate.
