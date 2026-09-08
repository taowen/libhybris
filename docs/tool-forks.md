# Maintained tool forks

Build-time patches have moved into maintained forks. The builders pin full
commit IDs and record repository URLs, source-tree hashes, toolchains and
installed-file hashes. To change either tool, commit and push the fix in its
fork, update the pin here, then rebuild and run the relevant device probes.

SPIRV-Tools' debug-iterator fix lives in
[taowen/SPIRV-Tools, branch ardesk](https://github.com/taowen/SPIRV-Tools/tree/ardesk),
commit `94043c878ff46fc2d7d48084696ef5fb02f5e3ea`, based on upstream
`f289d047f49fb60488301ec62bafab85573668cc` (Vulkan SDK 1.4.309.0).
`FlattenDecorationPass` now advances past non-`OpName` debug entries, including
`OpMemberName`, instead of hanging on valid grouped modules. It preserves
decorations, validation checks and application debug information.

`tools/build-validation-layer.sh` builds the pinned VVL with that dependency
commit. The prebuilt archive from `tools/fetch-validation-layer.sh` remains the
original upstream binary for comparison and is affected by this issue.


GFXReconstruct's empty-submit fix is maintained in
[taowen/gfxreconstruct, branch ardesk](https://github.com/taowen/gfxreconstruct/tree/ardesk),
commit `e6865bedad471a7ffaada05b6b18b29d77da36ee`, based on upstream
`c2ff0eecc7a7f43aa236a5c98097a685b928b782`. `tools/build-capture-tools.sh`
checks out that commit directly and records its repository, revision, actual
source tree, compiler and installed file hashes. Future changes belong in the
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
