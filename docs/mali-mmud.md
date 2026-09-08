# Known-build Mali MMUD initialization

This compatibility handling is limited to `libGLES_mali.so` build-id
`5ac4efe8d6175298b273dbaeb8f9d28e5e508e72`. It changes the process-local
`property_get_int32` result for `vendor.debug.gpu.easy_check`, preserving
recognized existing controls and adding bit 3. Unknown positive encodings
remain unchanged. It does not write Android properties or manufacture loader
headers. The common hook is installed automatically unless explicitly disabled;
secure execution and other builds are excluded.

## Inspected control flow

Addresses below identify the copied driver ELF from the X300 baseline, not
exported function names. The binary's nearest exported names are misleading.

- The decoder at `0x1db11d0` reads the property once. It stores log level at
  `0x32fd9c0`, control bit 2 at `0x32fd9c4`, and bit 3 at `0x32fd9c8`.
- Shared initialization at `0x1db0ac0` calls that decoder behind a C++ guard.
  Call sites include `0x9debf8` (API selector 1), `0xa2bb90` (selector 2)
  and `0xb0de24` in the EGL path (selector 3). Therefore EGL-first startup can
  cache the property before the standard Vulkan ICD is used.
- `0xa22d68` and `0xa22f60` consume bit 3 through the cached byte at
  `0x32f1ad0`. When set, they skip `0xa23770` and retain the device object's
  byte at offset `0x118`. Otherwise they replace that byte with the private
  loader inspection result.
- The inspection reads the instance's preceding header as a pointer to
  Android loader data. The observed header is HAL magic `0x1cdc0de`, and
  the read at `0xa237bc` faults at `0x1cdc1de`.
- The retained byte subsequently gates the replacement compute-pipeline path
  at `0xa233d4`. The separate bit-2 control can bypass checks in pipeline
  substitution (`0xa236a8`); it is left unchanged. Skipping loader inspection
  does not mean disabling MMUD or its pipeline optimization.

The repeated crash snapshot is retained under baseline
`20260907T062706-9fb06554/debug-icd-ubo-e5bf41b3`. Its additional LLDB Python
commands compute addresses from `frame.GetPC() - 0xa237bc` and read the actual
control storage: bit 3 is zero and its C++ cache guard is initialized. The
preceding `debug-icd-ubo-d3cb545a` correctly observed the device byte as zero,
but its two compound backtick address expressions read instruction bytes at
PC instead of the requested globals; those reads are not control-state evidence.

## Device regressions, 2026-09-08

The independent probe adds `egl-vulkan` and `vulkan-egl` modes using the existing
GLES 3 clear/triangle readback and Vulkan UBO readback in one process. The EGL
DSO remains resident between phases. These check sequential initialization
order, not simultaneous contexts or inter-API resource sharing.

Automatic mode leaves the MMUD environment variable unset:

- Mali `20260908T182101-fb7d975e` and Redmi `20260908T182101-eef74f30`:
  version and both mixed initialization orders PASS.
- Same-built-library Mali negative `20260908T182140-8af7ca60`, with
  `--icd-mali-loader-quirk 0`: version PASS, both mixed orders and ordinary
  UBO CRASH/139. No MMUD hook activation is logged.
- Mali `20260908T181958-28a4e4b9` and Redmi `20260908T181958-03fb3c61`:
  standalone GLES 3, version, UBO, UBO validation and all three widget
  capture/replay cases PASS. The first mixed-mode launch omitted the EGL null
  platform configuration and crashed on both devices; those two cases remain
  failed in these runs. The corrected mixed-mode results are listed above.
- Mali Wayland `20260908T182203-a8525800` and XCB resize
  `20260908T182422-fff10323`: PASS with VVL/SyncVal and no explicit MMUD switch,
  attaching to the installed Ardesk compositor. They retain GPU readbacks,
  physical screenshots and unchanged compositor identity.

- Mali teapots `tests/wsi/build/results/mmud-teapots-20260908T182729`:
  GLX and Wayland EGL both pass hardware, shader, swap and normal-exit checks.
  `/proc/PID/maps` identifies the newly staged common library, and the process
  environment confirms the MMUD variable is absent. Both Android screenshots
  show the orange teapot within its decorated window. These are visual checks,
  not full-window pixel comparisons. The runner, logs and provenance are saved
  with the result. Ardesk, its Mesa and the APK remain installed unchanged.
  An earlier temporary launch (`mmud-teapots-20260908T182535`) forced an absolute
  HAL path under the guest rootfs shim and failed namespace access before
  instance creation. The passing run uses the product's normal HAL discovery.

These results establish the listed startup and rendering paths. They do not
establish arbitrary MMUD optimization behavior, performance, secure-execution
fault injection, other firmware builds or full Vulkan conformance.
