# Reproduce a saved baseline case

`debug-baseline.py` replays a case from an existing baseline result under
an Android AArch64 LLDB server. It uses that result's staged libraries and
probe, checks the phone's build fingerprint, creates a unique remote directory
and forwards a private host port. It does not install an APK or require root.
The saved command and stage are trusted inputs produced by `tests/baseline/run.py`.

```sh
python3 tools/debug-baseline.py \
  --serial 10AFA31610002QH \
  --result tests/baseline/build/results/20260907T062706-9fb06554 \
  --case icd-ubo \
  --lldb-server "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/21/lib/linux/aarch64/lldb-server"
```

Requires host Python 3, adb, LLDB with Python support, readelf and the NDK's
AArch64 lldb-server. `--lldb` selects the host client; the command above was
verified with the installed client and NDK 29 server. Other client/server
versions are not established by this evidence. The current AArch64 library
build already retains DWARF; no extra debug build mode is necessary.

The printed output directory contains:

- `job.json`, source manifests and `staged-sha256.json`: selected command,
  input identities, client version, tool hashes and the actual copied files.
  Absolute paths in staged JSON are relocated to this run's remote directory.
- `lldb.log` / `server.log`: process output, first stop, selected-thread
  registers and disassembly, stack memory, module lookup and all-thread stacks.
- `stop.json`: inferior exit/stop status and thread frames/registers. A normal
  exit is recorded as such; success of collection is not success of the case.
- On a stopped process, `maps.txt`, `mapped-files.json`, `sysroot/` and `stage/`:
  stopped mappings, copied ELF files, hashes/build IDs and symbol-load errors.
  Android's separate linker is not automatically visible to LLDB. The collector
  explicitly loads the mapped ELF modules with their load biases, recovering
  unwind information and available symbols. Closed-driver nearest-symbol names
  can be misleading; retain the address and module/file offset as evidence.
- `memory.json` / `memory-*.bin`: bounded snapshots at readable selected-thread
  register addresses (512 bytes) and stack (16 KiB). This is not a full core dump.

Repeat `--stop-command 'LLDB command'` for additional inspection after symbol
loading; the commands are recorded in `job.json`. For the observed X300 crash:

```sh
# Append to the command above. These expressions are specific to this stop.
--stop-command 'memory read -s8 -fx -c40 `$x8-8`' \
--stop-command 'memory read -s8 -fx -c40 `*(unsigned long long*)($x0+0xc8)-8`'
```

The collector suppresses the probe's SIGALRM watchdog during debugging;
`--timeout` bounds the host LLDB session (default 90 seconds). It terminates
the inspected process after collection and removes its own remote directory,
server and port forward. Timeout evidence can be incomplete and is an error.
Mappings, files and hashes are observations at collection time, not proof
that executable memory equals the ELF files; TLS patching modifies memory.
This workflow has not established signal-handler, fork, native APK or GPU
kernel-crash coverage. Redmi root is available for later kernel/tombstone
investigation; this workflow runs as shell on both Redmi and unrooted X300.

## Verified evidence, 2026-09-07

- Redmi `20260907T062705-eb2706e7/debug-icd-ubo-9084213c`: normal exit 0.
- X300 `20260907T062706-9fb06554/debug-icd-ubo-d6afd0a4`: SIGSEGV at Mali
  file offset `0xa237bc`, fault address `0x1cdc1de`, x21=`0x1cdc0de`.
  All 60 copied mapped modules loaded into LLDB without a reported load error.
  Unwinding reaches `ubo_draw_internal` / `ubo_probe` / main; vendor internal
  frames are partly unnamed. Both successful and crashing probes are retained.
- A one-second timeout run (`debug-icd-ubo-4ee577ef` under the X300 result)
  returns failure and leaves no debugger/inferior for that run or port forward.

The crashing sequence reads `[x0+0xc8]`, then `[value+0x38]`, then the preceding
8-byte header. The saved memory contains the HAL magic at that header and
physical-device identification data at the intermediate object. This supports
an Android loader data dependency in the vendor MMUD path; it is not evidence
of a corrected Vulkan pipeline. AOSP's `driver.h` SetDataInternal stores
InstanceData in dispatchable object headers:
https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/vulkan/libvulkan/driver.h
That source explains the reference loader model, not the exact firmware source.
No dispatch header or driver optimization setting is modified by this tool.
