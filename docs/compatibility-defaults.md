# Compatibility defaults

All implemented runtime compatibility switches are enabled by default.
Applications do not need an environment-variable list to activate them.
Native format support takes precedence; forced emulation remains opt-in.

| Variable | Default | Explicit disable |
| --- | --- | --- |
| HYBRIS_VULKAN_COMPAT_SCALED_VERTEX | Missing-format conversion | 0 |
| HYBRIS_VULKAN_COMPAT_PACKED_VERTEX | Missing-format conversion | 0 |
| HYBRIS_BC_TEXTURES | missing | 0 |
| HYBRIS_VULKAN_COMPAT_UNUSED_BUILTINS | 1 | 0 |
| HYBRIS_VULKAN_COMPAT_POINT_SIZE | 1 | 0 |
| HYBRIS_VULKAN_COMPAT_INOUT | 1 | 0 |
| HYBRIS_VULKAN_COMPAT_IMAGE_BOUNDS | 1 | 0 |
| HYBRIS_VULKAN_COMPAT_VERTEX_STORES | 1, restricted to the inspected driver | 0 |
| HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK | 1, restricted to the matching driver build | 0 |

The first seven switches remain disabled during secure execution. Existing
secure-execution behavior for vertex stores and the Mali loader quirk is
unchanged. Capability checks, shader eligibility checks and driver guards
remain in place. Debug logging, dumps, validation and the `force` values
are not enabled by this policy.

Default enablement is not a conformance claim. In particular, image-bounds
rewriting changes selected out-of-range zero results to an in-bounds value.
Unused-builtins cleanup is module-local and can affect cross-stage interface
matching. BC fallback and packed/instanced vertex conversion still have
limitations recorded in the baseline evidence; the expanded nonzero
first-instance/divisor workload is not fully passing.

Older dated audits and baseline reports describe the opt-in policy used for
those runs. Their results remain historical evidence, not tests of this
combined default configuration.

## Combined-default smoke check (2026-09-20)

Built in Linux and tested on X300 with Debian/anlabwc, Zink and the Mali
vendor Vulkan driver. Chromium launched without compatibility enablement
variables, GL version overrides or backend-selection flags. It reported
OpenGL 4.4, GPU compositing enabled and zero GPU process crashes. WebGL2
returned RGBA [255, 0, 0, 255], GL error zero and no context loss. After 100
window resizes, the original Android screenshot retained all twelve test tiles.
OpenCode Desktop also reported GPU compositing enabled and zero GPU crashes
on X300. Its WebGL2 red-pixel check passed, and the original Android screenshot
after 30 resizes showed no duplicated UI regions.

On Redmi (Adreno 650, LXQt/anlabwc), GLX pbuffer rendering passed. OpenCode
reported Zink/Turnip OpenGL 4.6, GPU compositing enabled and zero GPU crashes;
WebGL2 returned the same expected pixel without error or context loss.
Thirty resizes completed and the visible window content remained intact
(the hosted keyboard obscured part of the Android screenshot).
The vertex-format fallback mask was zero, preserving native format support.

These checks use the installed compatibility layer with no per-pass enablement
variables. They do not cover every compatibility pass, native Wayland clients
or anhyprland. The earlier expanded instancing failure is still unresolved.

A broader follow-up found that X300 OpenCode's Home page still develops
duplicated regions after resize, including in its own DevTools screenshot.
The passing check above covered New Session, not Home. Disabling INOUT,
IMAGE_BOUNDS, POINT_SIZE and UNUSED_BUILTINS together did not remove that
failure. A subsequent Vulkan capture isolated the failure to Mesa's threaded
render-pass metadata ordering, not these compatibility passes. Moving the
metadata transition before the next draw/clear fixed the Home-page duplication.
With default optimizations and compatibility settings, Home and New Session
each passed 180 resize cycles on both X300 and Redmi; GPU compositing stayed
enabled with zero reported GPU crashes. GLX pbuffer checks also passed. The
separate expanded instancing limitation above remains unresolved.

The standalone OpenCode diagnostic launcher needed a bash exec step after
dbus-run-session; direct execution failed to open ICU data before GPU
initialization. The startup-path discrepancy still needs separate investigation.
