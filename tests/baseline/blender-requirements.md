# Versioned Blender Vulkan diagnostics

These probes check startup requirements, not shader execution or rendering:

| Mode | Requirement profile |
| --- | --- |
| `blender-vk` | Blender 4.3, retained for existing investigations |
| `blender-vk-5.2` | Blender 5.2.1 revision `9e2066aef7ef` |
| `blender-vk-5.2-optional-vertex` | The same revision with upstream fix `09417042a1fa` backported |

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

X300 evidence, `build/results/20260910T152404-aad8839c`:

| Route/profile | Result |
| --- | --- |
| Native, 5.2.1 | Missing clip distance and vertex pipeline stores/atomics |
| Hybris compatibility layer, 4.3 | Startup requirements pass |
| Hybris compatibility layer, 5.2.1 | Missing vertex pipeline stores/atomics |
| Hybris compatibility layer, 5.2.1 plus upstream fix | Startup requirements pass |

The vendor and layer both report `vertexPipelineStoresAndAtomics=false`.
Existing clip-distance emulation accounts for the other difference. No feature
reporting override is used by these diagnostics.

[Blender's upstream fix](https://github.com/blender/blender/commit/09417042a1fa)
makes vertex pipeline stores/atomics optional because they were used only by
development debug drawing. It removes two mandatory startup checks, requests
the actual feature value when creating a device, and gates debug drawing on
that value. Merely removing one check or reporting an unsupported feature as
true is not equivalent to this fix. Passing the patched requirement profile
does not establish that an unmodified Blender binary can run, or that the
patched application renders correctly.
