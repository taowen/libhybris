# Versioned Blender Vulkan diagnostics

These probes check startup requirements, not shader execution or rendering:

| Mode | Requirement profile |
| --- | --- |
| `blender-vk` | Blender 4.3, retained for existing investigations |
| `blender-vk-5.2` | Blender 5.2.1 revision `9e2066aef7ef` |

Select a diagnostic explicitly using `tests/baseline/run.py --case`, with
the usual runtime, manifest and device arguments. Both native Android and
standard-loader ICD routes are available, for example
`--case native-blender-vk-5.2 --case icd-blender-vk-5.2`.
They are excluded from the default regression suite because missing application
requirements are expected on some devices. Exit 1 means requirements are
missing; exit 2 means the diagnostic itself could not complete.

The probe resolves instance commands only after creating an instance. The old
probe attempted to resolve `vkDestroyInstance` through a null instance, which
the standard Vulkan loader correctly rejects. That failure did not diagnose
Blender compatibility. Vulkan 1.1/1.2 feature queries now include shader draw
parameters, timeline semaphores and buffer device addresses. The 5.2 profile
also checks vertex pipeline stores/atomics and the provoking-vertex extension.
Output identifies the selected profile and lists every missing requirement.

X300 evidence before the vertex-store implementation,
`build/results/20260910T152404-aad8839c`:

| Route/profile | Result |
| --- | --- |
| Native, 5.2.1 | Missing clip distance and vertex pipeline stores/atomics |
| Hybris compatibility layer, 4.3 | Startup requirements pass |
| Hybris compatibility layer, 5.2.1 | Missing vertex pipeline stores/atomics |

In that baseline, the vendor and layer both report `vertexPipelineStoresAndAtomics=false`.
Existing clip-distance emulation accounts for the other difference. No feature
reporting override is used by these diagnostics.

[Blender's upstream analysis](https://github.com/blender/blender/commit/09417042a1fa)
identifies this requirement as supporting development debug drawing. The
installed community 5.2.1 binary nevertheless requires the feature. Application
binaries remain unmodified: compatibility must be supplied by this fork, and
actual storage/atomic execution must be verified independently before changing
capability reporting. A newer application's relaxed startup requirements do
not establish compatibility of the installed package.

After the bounded G1-Ultra compensation, run
`build/results/20260910T162248-f9f2d899` passes the original 5.2 requirement
profile and both vertex-store validation routes (12 readbacks each, zero
validation errors). See [execution evidence and limits](vertex-stores.md).
The actual application also passes model/save/reopen and Workbench rendering;
Eevee remains failed. The startup probe alone does not establish application
rendering support.
