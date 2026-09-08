# Focused capture/replay and command outcomes

Capture no longer requires unrelated baseline cases. For example:

```sh
python3 tests/baseline/run.py --serial SERIAL --icd-hal HAL \
  --vulkan-loader LOADER \
  --capture-tools tests/baseline/build/gfxreconstruct/install \
  --case native-ubo --case hybris-ubo
```

Use the existing scoped loader quirk on the supported Mali driver. The runner
automatically schedules ICD version discovery and the ICD widget dependency.
`device.json` retains both requested `selected_cases` and actual
`scheduled_cases`. Ordinary, single-dynamic and [multi-dynamic](widget-multi.md)
UBO capture/replay each execute
correct and alternate bindings, independent reference renders, API conversion,
resource dumps, shader/layout association and exact full-image comparisons.
The probe-manifest-checked shader snapshots remain mandatory.

Each of the 24 reference/capture/convert/replay commands records its raw
`exit_code`, `timed_out` flag and elapsed seconds in command metadata. A host
observation timeout has no process exit code; it is not reported as zero.
The command log and existing suite-level failure/timeout classification remain
available. Successful capture stages were exercised here; no new capture-stage
crash or timeout was injected.

Capture child processes now merge stderr into stdout on the device before adb
transport, matching the ordinary probe launch. This prevents the cross-stream
splicing observed in the earlier scaled-format run
`20260907T191231-f953599e`. That is a baseline transport reproducer, not a
capture-specific failure. A separate 512-line alternating-stream exercise in
`build/capture-output-transport/evidence.json` matched all ordered bytes on both
devices with both the old and new launch; it did not reproduce the failure.

The earlier two-suite capture implementation was verified as follows.
Actual incremental library build completed in 9.732 seconds, followed by
Bionic/glibc/linked probe builds. Adreno run `20260907T192405-125fb0c9` and Mali
run `20260907T192407-f3c97627` each record 6 PASS: version, native/frontend/ICD
widget and both capture/replay suites. Each run records all 16 command exit
codes as zero and validates 12 image artifacts (four binding/mode combinations,
reference/captured/replayed), each exactly 1024 bytes. The first attachment
divergence remains draw 60 for ordinary binding and draw 61 for dynamic binding.
These are fixed headless workloads; no WSI frame or arbitrary application
capture gate is closed.
