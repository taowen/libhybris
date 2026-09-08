# Frontend window deletion (2026-09-08)

Desktop windows now have two product backends: standard loader → hybris ICD
and standard loader → Turnip WSI. The following frontend code was deleted,
not retained behind a build switch:

- EGL Wayland/X11 plugins, including PRESENT_SOCKET and forced timeout release.
- The EGL config-attribute override used only by the X11 plugin, and Wayland
  platform extension advertising in EGL/GLVND.
- The entire Vulkan frontend window-plugin loader and its null/Wayland plugins.
- The frontend window branch and environment selection in the WSI runner.

The ICD's existing Wayland native-window implementation moved unchanged to
`hybris/vulkan/icd/`. Its X11 implementation includes the shared Ardesk protocol
header directly. The replacement Vulkan library remains a headless diagnostic
path; it forwards global queries directly to Android's resolver. Its direct
swapchain export still checks the device resolver, since the Android ELF export
can otherwise bypass extension enablement. No desktop surface implementation
remains there. EGL null/fbdev/hwcomposer are outside the removed desktop plugins.
The install verifier rejects obsolete plugins, including stale staging.

The X11 client now collects its own ELF dependency closure using the existing
`stage-runtime.py` and the baseline builder's library search order. The runner
verifies hashes and rejects conflicting SONAMEs. It no longer obtains libX11
indirectly from the deleted EGL plugin. Version-query failures retain stderr.

## Build and deletion checks

Actual clean AArch64 builds and a subsequent incremental resolver fix completed
in `/tmp/libhybris-retired-frontends`. The final cleanup build was clean;
Wayland and X11 clients were also compiled. Existing headless probe binaries
were reused with their verified source/binary manifest; no unit tests were added.
The final installed ICD SHA-256 is
`56a8df83ccec5709d0094804a4abe4835ae22ed31e9ac8c1a396b1373fff0c9b`.
The frontend Vulkan SHA-256 is
`233a5ed0ff095c9c62215e6d0424be08f5a345baf1dc835f53322fa725221d54`.
Both are byte-identical before/after the final EGL-only hook cleanup.

`deletion-audit.json` verifies the final installation manifest and absence of
all six frontend Wayland/XCB/Xlib surface/presentation exports. Only the
null/fbdev/hwcomposer EGL plugins remain installed. The old runner invocation
without HAL/loader is rejected before contacting a device. Python compilation,
shell syntax and diff checks pass.

## Device evidence

Both devices use their vendor HAL through the ICD for these window runs; these
are **not Turnip window results**. The external service is installed
`io.taowen.ardesk`. VVL is the pinned 1.4.362 build; capture runs separately with
the recorded GFXReconstruct build. Results are under the build's
`baseline-results/`, `window-results/` and `window-controls/` directories.

| Device / run | Result |
| --- | --- |
| Mali `20260908T210526-254041ec`; Adreno `20260908T210526-a27fd035` | 12/12 each: frontend Vulkan query/draw/dispatch/init, disabled-WSI rejection, EGL and ICD version/draw/dispatch/lifecycle |
| Final EGL cleanup: Mali `20260908T211523-0dc3f1d5`; Adreno `20260908T211523-091f758f` | GLES2 pixel draw and EGL lifecycle both PASS |
| Wayland + VVL: Mali `20260908T210622-d6ac5343`; Adreno `20260908T210622-d17265f1` | PASS: 24 frame readbacks, three sizes, six screenshots, zero validation errors |
| Wayland capture: Mali `20260908T210927-223adcc4`; Adreno `20260908T210927-9cf5f5fa` | PASS: 24 copies/presents; six image checkpoints match virtual-swapchain replay. Replay is not a second compositor presentation |
| Mali XCB present `20260908T211433-3349eb34`; Adreno Xlib present `20260908T211433-5424be78` | PASS: eight frame readbacks and two screenshots each, zero validation errors |
| XCB resize: Mali `20260908T211222-d5b3aac6`; Adreno `20260908T211222-7568e82b` | FAIL: all 24 GPU readbacks exact, but two `VUID-vkQueueSubmit-pSignalSemaphores-00067` errors each |
| Pre-deletion XCB resize controls: Mali `20260908T211349-9e842e4c`; Adreno `20260908T211349-f2655df1` | Same two validation errors each with the previous libraries and runner; this failure predates deletion |

The resize failure occurs when the existing probe re-signals the semaphore
after the rejected out-of-date present. It remains an unresolved validation
gate; pixel success does not override it. Initial failed runs also remain:
`210331-*` exposed the lost direct-export guard (fixed and rerun);
`210714-*` exposed missing client libX11; `211001-e2255697` and
`211014-8c94d74e` rejected mismatched libc search origins before device execution.
The final X11 builder uses the same search order as the baseline builder.

This closes frontend window deletion and removes its implicit client dependency.
Cross-backend WSI contracts, Turnip product-window capture and application
acceptance remain open; G06/G12 are not marked complete.
