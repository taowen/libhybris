# Scaled format decisions and native capability evidence

`compat/scaled_formats.c` now owns scaled-format configuration, format pairs,
query adjustment and device conversion-mask decisions. Shader capture and
pipeline rewriting remain in `scaled_dispatch.c`. Query adjustment and device
creation share the same decision function; the format policy is unchanged.

With experimental scaled compatibility enabled,
`HYBRIS_VULKAN_COMPAT_FORMAT_TRACE=1` records the original scaled-format flags,
the integer-fetch format flags, the effective flags, and a reason for each of
the 12 format pairs. Each flags triplet is linear tiling, optimal tiling and
buffer features. Only the vertex-buffer bit may be added; image flags are
preserved. Reasons distinguish native scaled support, missing scaled fetch,
forced integer fetch, and unavailable integer fetch. The existing device mask
is derived from these same decisions.

The trace is disabled by default and ignored in secure execution. It runs
only at device creation, with at most 16 device snapshots (192 format rows)
and one truncation notice per process. It allocates no trace buffers and does
not hold a registry lock while calling the driver. Device/physical handles
identify a snapshot; they are not persistent object IDs or generations.
The older `HYBRIS_SCALED_VERTEX` mask message is separate and remains unchanged.

The existing independent pixel probe now prints all three legacy and
properties2 fields plus the integer-format query for each scaled format,
including formats that do not support a draw. It checks the two query forms
field by field. For an audited run, add:

```sh
--scaled-vertex-compat missing --scaled-format-trace \
--case native-scaled-vertex --case icd-scaled-vertex
```

Use `force` for the Mali conversion control. Include the appropriate device,
HAL and standard-loader options from the baseline README. The runner writes
format decisions under `formats` in each scaled shader evidence JSON.
`format_evidence.py` checks format identities, raw-to-effective changes, reasons,
mask, and the values observed by the application. If the corresponding native
case ran, it also checks the raw scaled/integer flags against that separate
native execution. A missing native reference is explicitly recorded as
`not available`; it does not claim an independent raw comparison. This audit
covers legacy/properties2 flags, not the 64-bit properties3 extension chain.

The runner now merges stderr into stdout on the device before adb transports
the probe output. Host-side stream merging could splice later stdout into the
middle of a diagnostic line. Adreno run `20260907T191231-f953599e` records that
failure: rendering returned zero, but the format auditor rejected the damaged
first row and the runner classified the case FAIL. That log remains unchanged.
Mali's concurrent initial run `20260907T191232-c3328824` passed; neither result
is used as a replacement for the final runs below.

Actual library build completed in 49.014 seconds, followed by successful
Bionic, glibc and linked probe builds. Source bytes match the retained build
inputs. Final runs under `build/results/`:

| Run | Evidence |
| --- | --- |
| `20260907T191415-3bb799d8` | Adreno 29854870, missing mode: 7 PASS / 2 UNSUPPORTED. All 12 decisions are `missing-scaled-fetch`; raw flags match native, effective flags match both application query forms. Native/frontend scaled draws remain unsupported. |
| `20260907T191416-3b700f6c` | Mali 10AFA31610002QH, forced mode: 9 PASS. All 12 decisions are `forced-integer-fetch`; raw flags match native and effective flags retain the existing values. |
| `20260907T191453-b18ecf4c` | Mali missing mode: 4 PASS. All 12 decisions are `native-scaled`, mask zero, no converted modules; native comparison passes. |
| `20260907T191452-befc88a0` | Adreno tracing disabled: 2 PASS / 2 UNSUPPORTED. Converted draw passes and no format-decision trace is emitted. |

Additional controls: Adreno disabled mode `20260907T191608-6419934b` has 1 PASS /
3 UNSUPPORTED with no compatibility/format trace, and Mali quiet forced mode
`20260907T191609-1fa0a18f` has 4 PASS with no format trace.

Each primary run includes lifecycle, ordinary scaled, specialization,
multi-entry decoration groups and instance-divisor validation regressions.
Legal rendering reports zero pixel failures and zero validation errors. Both
lifecycle logs contain exactly 192 format rows and one truncation notice while
device lifecycle evidence still passes. Changing raw/effective/application
query flags together in a copy of the Adreno log preserves internal policy
consistency but is rejected against the independent native query log.

No tested device lacks the paired integer vertex format, so the
`integer-fetch-unavailable` branch has no device evidence. Secure execution,
multiple physical devices, handle-reuse identity, other compatibility policies,
all image creation combinations and full applications remain outside this
coverage. G05 and the general format-compatibility gates remain open.
