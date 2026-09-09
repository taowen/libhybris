# Blender opaque composition on Redmi — 2026-09-09

The transparent background is fixed in the Ardesk window transport. Redmi's
installed product Mesa and Blender launcher now use the shared compatibility
layer and the matching Xwayland. The saved edited model opens with an opaque
viewport. **Full Blender acceptance remains FAIL:** default-size startup still
stops after failed acquire; this batch does not repair Blender's acquire logic.

## Contract and implementation

The Ardesk WSI advertised only `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`, but sent
RGBA/BGRA buffers through a premultiplied-alpha compositor path. Valid application
alpha zero therefore leaked the windows underneath. Buffer readback alone could
not detect this failure.

TAWC-DRI 0.4 adds `PresentBuffer2` with an explicit OPAQUE flag. Xwayland retains
that mode with each queued buffer and applies its opaque region at commit.
Legacy `PresentBuffer` and flags zero clear the region, preserving existing
premultiplied composition even for a 24-bit X window. Inferring the mode from
X visual depth would break the existing native-buffer scene workload.

Mesa negotiates version 0.4 for X11 presentation and sends the explicit flag.
Direct Wayland presentation sets an opaque region in logical surface coordinates.
Swapchain creation rejects composite modes not advertised by this WSI. Neither
path rewrites alpha in shaders or buffer contents, adds a second present path,
or nests Turnip in the Android HAL adapter. The shared compatibility layer's
bytes are unchanged from the rendering-segment fix.

## Independent device evidence

- The existing AHB scene workload now includes a full-color, alpha-zero OPAQUE
  frame, followed by a legacy half-alpha frame on the same window. With the
  legacy negative-control option, the original four phases and restored-alpha
  phase pass, but the opaque frame mixes the underlying yellow strip into its
  RGB (expected 224/32/48, observed 255/236/48 at the first checked patch).
- With the new server, all six scene phases pass, including twelve simultaneous
  windows, stacking, resize, opaque alpha-zero output and restored transparency.
  These are quadrant-patch and background-extent checks, not an entire-scene
  pixel oracle. Runs: `20260909T174446-8cfd25db` (negative) and
  `20260909T174722-0058add0`.
- The existing Wayland Vulkan probe now alternates alpha zero/one under OPAQUE.
  All 24 raw-image checks preserve that alpha. The physical green/red transitions
  cover all 254,976 pixels across 320×240, 448×288 and 256×192 windows.
  Fixed-driver ordinary run `20260909T175153-a90da292` passes; SyncVal run
  `20260909T175439-b78aa1b5` passes with zero errors. The prior descriptor-fixed
  driver passes raw readback but fails the physical window check at epoch zero.
- A malformed-flags request initially left its native-handle FDs queued for the
  next request: the following opaque patch read 112/16/24 instead of 224/32/48,
  and the restored-alpha phase also failed (`20260909T180811-146fc480`). Xwayland
  now registers the FD count before flag validation so DIX discards rejected
  handles. `20260909T181116-509fc977` verifies BadValue followed by correct
  opaque and restored-alpha frames; all six phases pass.
- After product deployment, both GLX and Wayland EGL teapot create/render/resize
  gates pass: `20260909T180115-3ec24b9e`. After installing and restarting the
  final component APK, both pass again in `20260909T181336-897045fc`.

The probe requires advertised OPAQUE support instead of falling back to INHERIT;
its independent screenshot checker still expects opaque RGB while verifying
alpha zero in the captured raw image. X11's existing checker default remains
alpha 255. These are extensions of existing device workloads, not unit tests.
The new alpha-zero Wayland gate has not yet been rerun against the hybris HAL ICD;
its earlier alpha-one passes do not establish this added coverage.

## Product integration and remaining failure

`tools/install-guest-gpu.sh` packages the standard layer in
`usr/lib/ardesk/vulkan` for both GPU overlays. It takes the supplied libhybris
library directory, or `HYBRIS_COMPAT_LAYER_DIR`. The Blender launcher enables the
layer without adding the Android Vulkan frontend to the library search path.
Other driver profiles pass through. Existing explicitly ordered layer lists
retain their order when the compatibility layer is already present.

The final Redmi test APK is derived from the installed APK and changes exactly
four payload entries: Xwayland, the Qualcomm GPU archive and its ID, and the
Blender launcher asset. The signing certificate is unchanged. The overlay uses
the actual staging script's Mesa, shared layer and dependencies. The startup
installer's overlay ID and the launcher hash are verified after restarting the
app; updating the asset matters because startup recopies the launcher.
This is a tested component APK update, not a full Gradle rebuild or fresh-install
qualification. Backups of the earlier APK and manual deployment are retained.
Maps and device hashes confirm live Blender uses `rootfs/usr/lib/mesa` and
`rootfs/usr/lib/ardesk/vulkan`, without a staged driver override. The final
screenshot shows `interactive.blend` in the 3D viewport with no terminal text
leaking through it.

A fresh default-size product launch still displays no window. The below-layer
capture ends at call 206, `vkAcquireNextImageKHR`, returning
`VK_ERROR_OUT_OF_DATE_KHR` for swapchain 10 and fence 14. The process remains in
`futex_wait_queue_me`. Blender 4.3.2's `GHOST_ContextVK::swapBuffers()` discards
the acquire result and then waits on that fence. The capture does not record a
completed subsequent wait. The final component APK also reproduces the absent
default-size window and futex wait. No fake successful acquire or premature fence signal
has been introduced to mask this application failure.

## Build and artifact identities

- Mesa source: `dc9853483cd37abc7b7ddadcc5af5fc5dc9a8add`; raw driver SHA-256
  `ce43f4f4eb4096389f00b36896efd92171f4ffdf34ee018739d3a180ee49cfb0`.
  Product RPATH staging produces
  `466bd4d6762e91c2df2644936968dfbdc7f6581b539190f126c7f2382dc2989e`.
- Initial Xwayland source: `cc88c97e1`, with SHA-256
  `643d50a85a8c4b34b5810add4a7eaae5af993a2b4a855f8617d018d9315bc982`.
  The FD-rejection fix is `dd0a97353`, final SHA-256
  `bcc695f39a2ca3141517a085817847f20dd2e943360ebb126df7c57a49cc729d`.
- Unchanged compatibility layer:
  `1e8ad39ac9a574e7d118fc47fc21f9446e7d3f0f6aeb42cb0355056b1df8de23`.

Mesa and Xwayland were built with the repository builders; the Wayland probe
was compiled with its existing builder. Ignored evidence under Ardesk
`build/blender-vulkan/redmi-opaque-*` retains build logs, source/runtime manifests,
APK comparison, deployment hashes, negative/positive physical screenshots,
raw images, validation logs, API capture and final product maps. The isolated
WSI runner manifests retain their base runtime identity and explicitly record
the replaced Vulkan driver under `driver_override`; other dependency libraries
were not rebuilt for that comparison. Full Gradle packaging, fractional scaling,
other-device alpha-zero regressions and Blender's complete application gate
remain unverified.
