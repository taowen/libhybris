# Vortek compatibility scope audit (2026-09-08)

This is a source comparison, not a claim of Vortek or libhybris conformance.
The reviewed host source is the local `x11-glibc-apk/app/src/main/cpp/` tree.
Its three files below are byte-identical to the extracted upstream archive
`winlator-app-c2f4ad4534f4637b543a9a3b085e28f50cf6d01c` in the workspace.
The old `ardesk/android/...` links in gaps.md no longer identify existing files.

| Host source under `vortekrenderer/src/` | SHA-256 |
| --- | --- |
| `shader_inspector.c` | `46c6a79fe4226e434c513850e07a481f51b41a2165f2b0c13ab2b4855d2c0523` |
| `texture_decoder.c` | `f9849a8764407f33b1e94d7850ebeaf426c3e78c0e94ad59f098beafe57d990b` |
| `timeline_semaphore.c` | `f18ef8e1c65e6e9729c420f38d2efdbf3c3255c4bca1df21e001e64b3057dd96` |

## Actual mechanisms and current status

| Mechanism | Vortek implementation | libhybris status / next decision |
| --- | --- | --- |
| Scaled vertex formats | Integer fetch plus shader conversion; enabled by device/format conditions | Implemented opt-in with independent rendering, aggregate, specialization and multiple-entry evidence. Remaining G08 generality is broader than this algorithm. |
| BC decode/upload | `isCanDecompressFormat` and `getBCInfo` cover BC1–BC5 | Implemented opt-in; BC6H/BC7 also implemented. Existing image failures remain open. A generic `isCompressedFormat` switch mentioning BC6H/BC7 does not prove those decoders exist in Vortek. |
| Clip distance | Removes ClipDistance capability and direct BuiltIn decoration when the physical feature is absent | Not imported. This does not implement clipping or prove safe removal of actual uses. Inactive declarations and active clipping must be handled separately under G08. |
| Point size | For DXVK, finds a store of constant 1.0 to a direct output and changes that variable to Private | Not imported. Finding one such store does not prove all stores are constant or that point rasterization can ignore the output. Any implementation needs the pipeline topology and complete use analysis. |
| Vertex/fragment interface widths | For DXVK, changes a fragment input pointer to a vector with the larger stage component count | Not imported. Associated load types, aggregates and interface rules need preservation; the source transformation alone is not a correctness oracle. |
| Image bounds | For Mali+DXVK, rewrites a selected zero alternative of OpSelect to the nonzero alternative after image operations | Not imported. It changes out-of-range behavior; do not apply as a general shader optimization without an identified workload and preserved semantics. |
| Timeline waits | Deserializes a wait request, runs **native** `vkWaitSemaphores` on a worker, sends status via eventfd | Native core/KHR timeline dispatch is already implemented and tested where supported. This file is RPC wait transport, **not software timeline emulation**. An emulator remains a separate G09 design problem, not a missing port of this file. |

Vortek therefore supplies two substantial compatibility algorithms already
present here and four conditional shader edits needing individual decisions.
It does not justify treating software timeline emulation as a ready-made port,
or spending further BC work as a prerequisite to examining the remaining shader
and application paths. Conversely, this audit does not close G02–G13: their
acceptance criteria include broader correctness, tools, lifecycle and application
coverage than these source files.

Current device caps from runs `20260908T134750-4e7ad5e7` (Redmi) and
`20260908T134750-d55ce8dc` (Mali) show clip/cull=true on Redmi and false on Mali;
both expose largePoints. These are capability queries, not clipping/point
rendering evidence. A useful next shader batch must distinguish active builtins
from removable declarations and retain an unsupported result for semantics it
cannot implement, rather than blindly applying the Vortek edits.

This audit changes documentation only. It was checked against both local source
copies and the recorded device capability logs; no runtime build or pixel
regression was required or claimed for this correction.
