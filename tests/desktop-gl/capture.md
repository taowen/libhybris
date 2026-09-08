# Vulkan capture of desktop GL workloads

Add `--capture-tools /path/to/gfxreconstruct/install` to `run.py`. Use the pinned
install produced by `tools/build-capture-tools.sh`; each installed file is
verified against its manifest before staging. The shared staging helper also
serves the baseline capture workloads. Capture and validation use separate runs.

For the failing Mali attribute workload:

```sh
python3 tests/desktop-gl/run.py \
  --serial 10AFA31610002QH --hal /vendor/lib64/hw/vulkan.mali.so \
  --api-version 1.3.305 --mali-loader-quirk --packed-vertex 1 \
  --profile core33 --vertex-prepass --vertex-draws \
  --capture-tools tests/baseline/build/gfxreconstruct/install
```

The runner records through the standard Vulkan loader, above the ICD. It saves
`capture/desktop.gfxr` even when rendering fails, checks its SHA-256 against the
device file, and converts it to JSONL with the pinned tool. `capture/commands.json`
retains the conversion command and exit status; `convert.log` retains errors.
The GFXReconstruct layer must appear in the process mappings. Converter and
probe cleanup are restricted to their PIDs in the unique run directory.

`capture/vertex-draws.json` indexes recorded draw commands by command-buffer
begin, bound pipeline/create call, vertex attributes, buffers, offsets, effective
strides and divisors. Successful queue-submit calls are linked to the recorded
commands. These references prove submission, not GPU completion or correct
pixels. The original GL image and error checks still decide rendering status.
`probe_exit_code`, aggregate `status`, `capture.status` and
`capture.vertex_analysis_status` remain separate in `result.json`.

This is a vertex-state index, not complete pipeline/resource replay. Indirect
argument bytes, linked pipeline-library state, shader objects and secondary
command-buffer execution are not decoded. Unknown linked/shader-object state is
marked incomplete; indirect calls retain buffer IDs, offsets, counts and stride
without inventing firstInstance values. The full standard capture and converted
calls remain available for further analysis. No desktop replay is performed.

## Device evidence

The AArch64 runtime was built, Python entry points compiled, and the pinned
capture tools' hashes verified on 2026-09-08. Mesa and the GL probe are unchanged.

| Run | Workload | Rendering | Capture / vertex index |
| --- | --- | --- | --- |
| `20260908T164123-49a6acf7` | Mali core 3.3, packed option, main + twelve packed draws | PASS | 1,606 Vulkan calls; 13 recorded/submitted draw commands; vertex state complete |
| `20260908T164330-203aa6db` | Mali, plus explicit compute and expanded vertex draws | FAIL | 3,582 Vulkan calls; 27 recorded/submitted draw commands; vertex state complete |
| `20260908T164331-adf76c1c` | Redmi / Turnip, expanded workload | PASS | 7,262 Vulkan calls; 50 recorded/submitted draw commands; vertex analysis PARTIAL because pipeline libraries are not resolved |

The final Mali capture preserves the same eight attribute failures as the
uncaptured validation run. Four direct commands explicitly combine
firstInstance=5 with instance binding 1/divisor=2: call indices 2508, 2753, 3594
and 3714, corresponding to attribute phases 1, 3, 9 and 10. Their effective
strides are 8, 8, 5 and 8 bytes. This is a combination the
[native KHR property reports unsupported](../baseline/capabilities.md#legacy-divisor-properties-on-khr-only-devices).
The other four failing phases use indirect-count commands; their actual argument
bytes have not yet been decoded. Capturing these calls does not repair them.

The extracted staging helper also passes the independent Adreno baseline run
`20260908T164124-ade5672e`: version, UBO and all three good/bad capture-replay
pairs pass (5 PASS). Those baseline pairs retain exact reference/capture/replay
pixels and descriptor/shader/attachment evidence. Their replay success does not
establish replay support for the desktop captures above.
