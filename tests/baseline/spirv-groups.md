# Grouped SPIR-V and validation-layer evidence

Grouped decorations and the fixed validation-layer dependency used by [scaled vertex probes](scaled-vertex.md). Original tool failures and final successful runs are retained separately.

## Grouped SPIR-V decorations and validation tooling

`scaled-vertex-group`, `scaled-vertex-group-multi` and
`scaled-vertex-group-spec` encode the existing single-entry, three-entry and
specialized-array shaders using decoration groups. The fixtures group Location,
Flat, Block, member Offset and SpecId decorations; BuiltIn remains explicit.
The generator helper preserves the other instruction words. Independently
flattening each fixture with SPIRV-Tools reproduces the original decoration
multiset and all non-debug instructions. The three fixtures retain member debug
names in the actual modules supplied to Vulkan.

`compat/spirv_decorations.c` expands group applications before entry extraction
and scaled conversion. It retains the original group declarations and names,
which become unused collectors after applications are expanded. The existing
passes can then see direct input locations, aggregate layouts and specialization
IDs. The separate pass checks instruction lengths, group references and output
size arithmetic, and uses the application allocation callbacks. Grouped string
annotations and grouped ID operands applied to members remain unsupported;
legacy grouped OpDecorateId to variables is not GPU-covered by these fixtures.

The probe's shader selection and format data now live in `scaled_fixture.h`;
`probe_scaled.c` uses the selected shader's properties instead of a growing
variant-number conditional. All existing scaled fixtures use the same table.
The group shader auditor retains raw shader hashes/disassembly, then uses
SPIRV-Tools on a derived copy to compare direct decoration semantics before and
after conversion. Raw group applications must be present before conversion and
absent afterward. Original modules must still match the built fixture assets.

The pinned Debian VVL 1.4.309.0 hangs in its SPIRV-Tools decoration flattening
pass when it encounters `OpMemberName`. The three new validation cases reach
shader creation and expire the probe alarm before entering the adapter's
conversion. Adreno run `20260907T182242-02cf310c` records 13 PASS / 3 FAIL;
Mali `20260907T182243-d6086e71` records 16 PASS / 3 FAIL, including all three
native group controls passing. These failures are retained as tool failures.
The same iterator issue was reproduced in host SPIRV-Tools 1.4.341.0; audit
normalization strips debug only from its derived copy before flattening, with
a 30-second subprocess limit. The raw shader/debug evidence is retained.

Build a validation layer with the missing iterator advance fixed:

```sh
tools/build-validation-layer.sh
```

The script pins VVL and its five build dependencies, applies the two-line
SPIRV-Tools patch, and builds AArch64 with the repository's fixed container
recipe. No validation rules are changed. Select its outputs explicitly:

```sh
python3 tests/baseline/run.py --serial SERIAL --icd-hal HAL \
  --vulkan-loader LOADER --scaled-vertex-compat missing \
  --validation-layer tests/baseline/build/validation-build/install/lib/libVkLayer_khronos_validation.so \
  --validation-manifest tests/baseline/build/validation-build/install/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
  --validation-build-manifest tests/baseline/build/validation-build/install/manifest.json \
  --case icd-scaled-vertex-group-validation \
  --case icd-scaled-vertex-group-multi-validation \
  --case icd-scaled-vertex-group-spec-validation
```

Use `force` plus the existing scoped loader quirk for the Mali conversion
control. The runner verifies the layer hash against the build manifest and
retains that manifest alongside the run. The old prebuilt validation fetch
remains available for comparison; its group-specific hang is not fixed by a
libhybris update.

Without a validation layer, final library runs
`20260907T182941-89884adc` (Adreno missing mode) and
`20260907T182624-73249531` (Mali forced mode) each have version and all three
group fixtures PASS. Each retains 84 original/converted pairs and 36
specialization-data dumps, with 216 complete image checks. The Adreno negative
control `20260907T182623-1f06edcc` uses archived `f4c3aaa` libraries with the
current probe: all three group cases fail conversion before submitting a module.
It is a negative control, not an acceptance pass.


Final patched-layer runs `20260907T184150-a31e3934` (Adreno missing mode) and
`20260907T184151-29e43de3` (Mali forced mode) each have **17 PASS**. All three
grouped draw cases finish with correct pixels, zero VVL/SyncVal errors and no
live callback allocations. The other scaled/aggregate/specialization fixtures,
UBO, lifecycle and allocator cases also pass. The deliberate invalid-buffer
control still reports exactly `VUID-VkBufferCreateInfo-size-00912`, returns
`VK_ERROR_VALIDATION_FAILED_EXT`, and records zero other errors. Each run retains
12 shader audits covering 276 original/converted module pairs; 84 pairs are the
new grouped fixtures. The copied validation build manifest matches the actual
layer hash and identifies all six source snapshots, the patch, compiler,
container and build configuration. The final exported source snapshots also
match their pre-build fingerprints. Mali native-pass-through run
`20260907T184356-155094a3` also has version and all three grouped validation
cases PASS with `fallback_mask=0` and no converted modules.

The runner now records the original `probe_exit_code` separately from shader
verification errors, and no longer turns a crash/timeout/unsupported process
result into an ordinary audit failure. Old-layer confirmation
`20260907T183901-b1f6851e` records the grouped shader's alarm as **TIMEOUT 142**,
with the missing-dump audit error recorded separately. Earlier failure records
above retain their original classification. This does not add CTS coverage or
prove all tool failure modes.

G04/G07/G08/G13 remain open: this is headless grouped-shader/tooling coverage,
not WSI capture/replay, arbitrary application diagnostics, general SPIR-V
extension/debug semantics, a complete format fallback or a multi-version CTS
baseline. The optional patched VVL build and the unchanged prebuilt archive
must be distinguished in future reports.
