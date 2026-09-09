# Headless GPU baseline

Compare the same small EGL/GLES and Vulkan workloads on one Android device:
`native` uses bionic and the Android stack; `hybris` uses glibc and the
replacement frontend. Shared C sources keep the workloads comparable.
No APK, root, rootfs, compositor or X server is required.

The baseline checks context creation, shader drawing and pixel readback,
Vulkan buffer fill/fence/readback, and reported capabilities. It is a smoke
test, not CTS or application compatibility certification. A reported API
version or feature is not evidence that its complete semantics were tested.

## Build and run

From the repository root, with Podman, adb and an Android NDK available:

```sh
bash tools/build-aarch64.sh
export ANDROID_NDK_HOME=/path/to/android-ndk
bash tests/baseline/build.sh
python3 tests/baseline/run.py --serial DEVICE_SERIAL \
  --case native-2 --case hybris-2 \
  --case native-3 --case hybris-3 \
  --case native-vk --case hybris-vk \
  --case native-caps --case hybris-caps
```

The library build uses the repository's pinned container and Android headers.
The probe build produces bionic, glibc and directly linked glibc executables.
[Build and provenance](build.md) describes prerequisites, custom paths,
incremental builds, manifests and standalone-build evidence.

`--case` is repeatable. Omitting it runs all applicable cases in this shared
runner, including extended ABI and compatibility regressions; that is larger
than the smoke test above. `python3 tests/baseline/run.py --help` lists options.
For the standard-loader/ICD route and optional validation/capture, use
[the loader and tools guide](loader-tools.md).

## Read the results

Results are stored in `build/results/<run-id>/`. Compare native and hybris
on the same device and retain the exact driver identity and run ID.

| Result | Meaning |
| --- | --- |
| PASS | That case's implemented assertions passed |
| UNSUPPORTED | A required capability or route was unavailable; execution is not covered |
| FAIL | Setup, an assertion or an evidence check failed |
| CRASH | The process terminated abnormally |
| TIMEOUT | A watchdog or host deadline stopped the case |

The runner exits nonzero for failures, crashes and timeouts. It verifies staged
ELF hashes against the build manifests and retains per-case logs, commands,
queried driver information and mapping snapshots. Android mapped-file hashes
are collected after execution. These snapshots do not prove every historical
mapping, live memory-page identity or driver unloading.

`device.json`, the staged manifests and each case's log are the evidence for
that run. Totals from older runs do not describe current HEAD or another device.
[Initial smoke results](smoke-results.md) are historical records, including
original failures and their limited coverage.

## Debug a saved failure

Use [tools/debug-baseline.py](../../tools/debug-baseline.py) to replay a saved
case under LLDB with its ELF files, stacks, registers and mappings.
[The debugging guide](../../tools/DEBUGGING.md) documents the commands and limits.

## Extended regression suites

The shared executable also contains lifecycle, TLS, synchronization,
shader/format and widget probes. Their usage and evidence are organized by
subject in [the test index](../README.md), rather than appended here.
Window presentation is tested by [WSI](../wsi/README.md); desktop OpenGL by
[the Mesa suite](../desktop-gl/README.md).

Keep this README as the smoke-test entry point. Put new feature requirements,
detailed experiments and dated device results in the relevant topic document.

## Descriptor-pool reset

`ubo-pool-reset` and `ubo-pool-empty-reset` reuse the UBO workload for eight
exact 16×16 pixel checks across pool reset, individual descriptor-set free,
reallocation and completed command-buffer reuse. The empty variant also resets
before the first allocation. Native, hybris and ICD cases are registered; ICD
`-validation` variants enable SyncVal through the existing validation option.
See [Turnip negative controls and fixed-driver results](../../docs/blender-turnip-descriptor-pools.md).
