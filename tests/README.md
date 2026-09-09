# Test suites

Choose a workload for the behavior being changed. The headless suites share
`baseline/build.sh`, `baseline/run.py` and probe sources for native/glibc
comparisons; sharing a runner does not make every case part of the baseline.
The [acceptance checklist](../gaps.md) remains authoritative for the full project.

## Entry points

| Suite | Purpose and entry point |
| --- | --- |
| Headless baseline | [Small EGL/GLES and Vulkan smoke test](baseline/README.md) |
| Desktop OpenGL | [Official Mesa/Zink, EGL and GLX](desktop-gl/README.md) |
| Window presentation | [Wayland/XCB/Xlib surfaces, swapchains and external compositor](wsi/README.md); X11 clients are built by `tests/x11` and run only through this runner |

## Headless regression topics

Select cases with repeated `--case` arguments to `baseline/run.py`.
Use [build/provenance](baseline/build.md) and
[standard loader, validation and capture setup](baseline/loader-tools.md)
for shared infrastructure. Each topic records its own prerequisites, measured
results and remaining gaps.

| Area | Documentation |
| --- | --- |
| Vulkan entry routes and aliases | [Dispatch](baseline/dispatch.md) |
| Feature, limit and query consistency | [Capabilities](baseline/capabilities.md) |
| Instance/device ownership and allocation refusal | [ICD lifecycle](baseline/icd-lifecycle.md) |
| EGL sharing, migration and concurrent drawing | [GLES contexts](baseline/gles-contexts.md) |
| Android TLS and foreign-thread first touch | [TLS](baseline/tls.md) |
| Synchronization object publication and destruction | [Initialization and ownership](baseline/sync-initialization.md) |
| Mutex/rwlock/condition clocks and waits | [Timeouts](baseline/sync-timeouts.md) |
| Stdio and hook-table behavior | [libc hooks](baseline/libc-hooks.md) |
| Timeline queues, non-coherent memory and partial mappings | [Memory and synchronization](baseline/memory-sync.md) |
| UBO layout, staging and descriptor updates | [Widget](baseline/widget.md), [multiple descriptors](baseline/widget-multi.md) |
| Draw-to-resource evidence | [Widget capture](baseline/widget-capture.md), [capture execution](baseline/capture-execution.md) |
| Native gralloc/AHB import | [Native buffer](baseline/native-buffer.md) |

## Opt-in compatibility probes

These exercise explicit compatibility options; their results are separate from
default-off smoke results. Read the documented limits before interpreting PASS.

| Area | Documentation |
| --- | --- |
| Scaled vertex fetch | [Conversion](baseline/scaled-vertex.md), [aggregates and specialization](baseline/scaled-aggregates.md) |
| Rendering segment preservation and layer ownership | [Shared Turnip/HAL transformation, pixel and allocator probes](../docs/vulkan-compat-layer.md) |
| Scaled policy and instancing | [Format decisions](baseline/scaled-format-policy.md), [divisors](baseline/scaled-instancing.md) |
| Packed SNORM vertex fetch | [Packed vertices](baseline/packed-vertex.md) |
| SPIR-V transformations | [Grouped decorations and validation-layer fix](baseline/spirv-groups.md), [unused builtins](baseline/unused-builtins.md), [PointSize](baseline/point-size.md) |
| BC decoder and image integration | [Decoder](baseline/bc-decode.md), [images](baseline/bc-images.md), [RGB borders](baseline/bc-rgb8.md) |
| BC6H and BC7 | [BC6H](baseline/bc6h.md), [BC7](baseline/bc7.md) |

[Initial smoke records](baseline/smoke-results.md) and
[window bridge history](baseline/window-bridge.md) retain earlier observations.
Historical PASS counts do not establish current whole-suite coverage.
