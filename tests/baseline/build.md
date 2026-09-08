# Build and provenance

Build commands, input identity, staging and compiler-cache evidence for the shared headless probe runner.

## Build

From this repository root:

```sh
bash tools/build-aarch64.sh
export ANDROID_NDK_HOME=/path/to/android-ndk
bash tests/baseline/build.sh
```

Choose the [baseline smoke cases](README.md#build-and-run) or a workload from
[the test index](../README.md) when running the artifacts.

`tools/build-aarch64.sh` works in an independent checkout. The default
builder is owned by this repository: `tools/ensure-builder.sh` uses
`tools/container/Containerfile.aarch64`, a digest-pinned Debian base and
the 2026-08-01 package snapshot. Host prerequisites are x86_64 Linux,
Podman, Git, tar, Python 3.10+, sha256sum, file and flock; running also needs adb.
The first build downloads the compiler and dependencies.

`tools/fetch-android-headers.sh` fetches Halium Android 11 headers at
`2c6ac3dcc4f8db593dd69906b0ec22822abfed91` and verifies cached content.
The default cache is `build/deps`; `HYBRIS_DEPS_DIR` may override it.
Use `--headers` for custom headers or `BUILDER_IMAGE` for a custom image.
Overrides record actual inputs but do not inherit the default recipe's
reproducibility claim.

For a local edit/build loop, add `--incremental` (and keep `--debug` if the
cache was built in debug mode). A successful build seeds the next compiler
cache; C/C++/assembly source edits are synchronized by content, even when a
checkout or editor preserves an old mtime. GNU make still visits the full
configured dependency graph, compiling/relinking only affected targets. This
avoids a hand-maintained list of dependent libraries or an unsafe target-only
shortcut. Headers, build rules/generators, added/removed files, builder/config
changes and build-script changes force a clean rebuild. `--clean` overrides
`--incremental`; omitting `--incremental` retains the clean-build behavior.

`inputs/` is the clean pre-build source snapshot; `src/` also holds generated
files and cached objects. Each build starts with empty `install/` and `runtime/`
directories and creates a new complete ELF manifest. The manifest records
`build_mode`; `build-report.json` records the cache decision, changed input
paths and elapsed time. Cache provenance is published only after a successful
build. The previous manifest/cache marker is withdrawn when preparation starts;
a failed/interrupted build is not a completed cache. A per-output flock rejects
concurrent builds into the same directory; use different `--out` directories
for independent configurations. This is a trusted local compiler cache, not
an independent source-to-binary reproducibility proof. Run a clean build and
applicable probes before accepting a compatibility change.

Fresh source/header snapshots are taken before compilation. The library
manifest contains their file hashes, the container image ID, compiler
version, installed package versions, configure arguments and staged ELF
sha256/build-id. The recipe and build script are also fingerprinted.
The source commit/dirty flag is supplementary to these content identities.
Staging rejects stale plugins such as `vulkanplatform_x11.so`.

`tests/baseline/build.sh` compiles `probe-glibc`, `probe-glibc-linked` (DT_NEEDED libvulkan)
and `probe-bionic` from a source snapshot. Set `ANDROID_NDK_HOME` or an
explicit `BIONIC_CC`. It uses the same pinned glibc builder by default;
`GLIBC_CC` is an explicit override. `bundle/probe-manifest.json` records
source hashes, compiler identity and the three executable hashes.

```sh
python3 tests/baseline/run.py --serial 29854870 \
  --hybris-lib /path/to/install/usr/lib/hybris \
  --runtime /path/to/runtime \
  --manifest /path/to/manifest.json
```

The runner uses a unique `/data/local/tmp/libhybris-baseline-<run-id>` directory,
verifies the supplied manifest against all staged ELF files (including SONAME
aliases), rejects unknown platform plugins, and kills its recorded probe PID
after a host timeout after checking its executable path. Manifest hashes describe
staged files. Per-case `*-mappings.json` records observed file-backed mappings
at completion and, for unload/TLS cases, before closing the frontend.
Staged paths are associated with their hashes. Android paths are hashed
after execution in `android-mapped-files.sha256`; errors are retained.
These snapshots do not capture every historical mapping or verify live
mapped pages. `device.json` includes commands and queried driver strings.
Custom library paths do not inherit
the default manifest; supply their matching manifest explicitly. Results go under `build/results/<run-id>/`.
When a probe manifest is supplied in the bundle, changed executables are
rejected before deployment and staging is checked again.
Each probe arms `alarm(25)` from its own constructor. A dependency constructor
can run earlier, so the host timeout and pre-exec PID tracking are still required.
Exit 0 means the implemented checks passed, 3 means unsupported, 124 timeout;
the runner returns nonzero for FAIL/TIMEOUT/CRASH.

## Source layout

probe.c dispatches modes and installs the watchdog. probe_common.c
contains symbol lookup, memory and queue selection helpers. EGL, Vulkan fill,
dispatch, lifecycle, capabilities and widget rendering live in separate
probe_*.c translation units. build.sh uses the same explicit source list
for bionic, glibc and the directly linked glibc variant.


## Standalone build verification

A temporary checkout outside the parent project used the repository's default
builder and downloaded headers, then built all library and probe artifacts.
The builder image ID was
`62d617eddf3720b5c3574f6666c59bcb2abb7deb03f74a6801820b20fc874704`;
the header snapshot hash was
`fd30a127eb85c3258cf471906cb002b1f1e110acdccb6247c5dc3aee3a1ddffc`.

- 29854870: `20260906T235359-6ecadb60`, 21 PASS / 2 UNSUPPORTED.
- KB2000: `20260906T235440-110adca9`, 21 PASS / 2 UNSUPPORTED.

Both unsupported cases are desktop GL. On 29854870 the TLS probe recorded
both before-close and completion mappings, including common and vendor
libraries. All 93 observed Android file paths were hashed without error.
Temporary modified-header and modified-executable checks were rejected.
These are standalone smoke results, not CTS or Blender regression results.

The final runner rerun on 29854870, `20260906T235759-0a9d4916`,
also completed with 21 PASS / 2 UNSUPPORTED, retaining all 23 commands,
nonempty mapping snapshots and driver/probe identities.

## Incremental build evidence

Incremental-build checkpoint (2026-09-07): `--incremental` preserves the
completed local compiler cache while staging a fresh install/runtime bundle.
Initial debug cold/no-change builds took 48.475/9.230 seconds. Two Wayland
source edits with deliberately preserved old mtimes rebuilt exactly three
objects (the common source is compiled into both EGL and Vulkan plugins),
changed only those two plugin ELFs, and completed in 11.936 seconds. X300
`20260907T085347-860b2245` passed the three-size/24-frame screen gate with
that incrementally rebuilt debug bundle. Tracepoints retain disconnect counts
and retired releases without per-hook debug arguments: 2,081 lines / 164,904
bytes versus the earlier verbose run's 153,855 lines / 12,813,149 bytes for
the same workload. This is an observed reduction, not a general size bound.

Final release cold/no-change builds took 46.868/9.437 seconds; all 75 deployed
ELF entries have identical hashes. X300 `20260907T085914-d8c8a645` passes
24 frames and 254,976 screen pixels; Redmi `20260907T085914-8f94a259` passes
eight missing-protocol rejections but remains window-UNSUPPORTED. Concurrent
same-output builds were rejected while the first build was running. Switching
debug/configuration/scripts forced a clean rebuild; the current output's
header snapshot was successfully reused as --headers input. Local build
reports, manifests, logs and object comparisons are retained under
`build/incremental-evidence`. Header/edit/add/delete and failure fallback
branches were reviewed but not separately exercised with full builds in this
batch. Compiler cache contents are trusted local state; no reproducibility
or arbitrary cache corruption guarantee is claimed. Independent compositor
isolation and automatic two-process failure capture remain unfinished.

Final full baseline: X300 `20260907T090014-cd3e02ef` is
135 PASS / 10 UNSUPPORTED / 1 CRASH; Redmi `20260907T090014-d22fcaa8` is
98 PASS / 47 UNSUPPORTED / 1 CRASH. Native-groups remains the sole crash on
both. Validation, SyncVal and both headless capture/replay gates pass; X300
ICD uses the scoped Mali option. No new compositor or APK was installed.
