# Shared product Vulkan window gates — 2026-09-08

`tests/wsi/run.py --backend hybris|turnip` selects the backend while retaining
one set of window clients, validation/capture handling and evidence checks.
`backend.py` stages and verifies the selected runtime, supplies the ICD for
version discovery, and checks the live loader/driver mappings. Turnip uses the
product Mesa runtime packaged by `tests/desktop-gl/build.sh`; there is no
second Mesa pin or window-specific build. The product owns its loader/libc
pair. Other X11 client/runtime SONAME conflicts remain errors.

X11 clients select an advertised opaque or inherit alpha mode; their test
pixels have alpha one. They log the actual usage, alpha and extent. This does
not make the two drivers' import capabilities or alpha policies equivalent.
Turnip needs the compositor Wayland allocator even for XCB/Xlib presentation.

## Inputs

- Turnip: Redmi `29854870`, Vulkan version discovered as `1.4.359`; Mesa
  `ae5de4494eb2a4aa8c7adcc116c0aa61bd6efbcb`, tree
  `a4855792f5dd34601c8ede84f764c18a5a0e1d10`. Driver SHA256
  `9ba2ddd45101adcd3ef4cc023262851fcf652890c6574a38c21d64a150c974bf`.
- Hybris: Mali X300 `10AFA31610002QH`, vendor `vulkan.mali.so`, discovered
  `1.3.305`; verified build `/tmp/libhybris-retired-frontends` from the
  [frontend removal batch](../../docs/stack-consolidation.md).
- Standard loader SHA256 on both paths:
  `22b6ce3145007566e3e47ce1f379135ba9b83e734cba2edc2d0c3e1b28ffeea7`.
- VVL `1.4.362`, SHA256
  `6b085cc9058769fde84f88870d64fa106f863f70e32e2d2baf16f87c785fbc7e`;
  validation runs enable SyncVal. Capture runs separately use the pinned
  GFXReconstruct build at `ab565b27`.

Mesa and both C window clients were actually rebuilt. The runs attach to the
already running `io.taowen.ardesk`; they do not install a new APK. Each run
retains APK identity, compositor identity checks, input manifests, source and
ELF hashes, live mappings, logs, readbacks and physical screenshots.
Raw records are under `/tmp/libhybris-product-wsi-results/<run>/`.

## Final runs

| Gate | Hybris / Mali | Product Turnip / Redmi |
| --- | --- | --- |
| Wayland present + validation | `20260908T212901-bbca962a` PASS | `20260908T214704-4f7ad45c` PASS |
| XCB resize + validation | `20260908T213345-29d77326` PASS | `20260908T214545-c1f92556` PASS |
| Xlib resize + validation | `20260908T213824-537504e8` PASS | `20260908T214632-921e7268` PASS |
| XCB acquire-timeout + validation | `20260908T214528-777c2d81` PASS | `20260908T214835-c387ea99` PASS |
| Wayland capture + virtual-swapchain replay | `20260908T213459-0e9d9312` PASS | `20260908T214755-53086e7c` PASS |

All validation runs report zero errors. Wayland checks 24 exact GPU readbacks,
three extents and six physical screenshots, plus the existing surface
lifetime workload. Each X11 resize run checks 24 readbacks, six screenshots,
two acquire/present out-of-date transitions, unchanged output index,
unsignaled fence, present-wait consumption, semaphore reuse and preservation
of old images. XCB and Xlib each record 24 presents: Mali observes 23 releases,
Turnip 19, with no reuse before release. These counts do not assert that all
buffers receive a release before client teardown.

Acquire-timeout holds all three images: zero timeout returns NOT_READY and a
finite wait returns TIMEOUT (Mali 20.85 ms, Turnip 20.23 ms). Both preserve the
output index and leave the fence unsignaled. Capture records 24 copies and
24 presents, with all six replay checkpoints byte-identical to live readback.
Mali uses RGBA for all epochs; Turnip uses BGRA/RGBA/BGRA. Replay uses a virtual
swapchain and does not constitute a second physical presentation.

## Retained failures and trace correction

Turnip `20260908T213140-4e5dd292` rendered eight frames with zero validation
errors but failed the unchanged X11 gate because the product driver had no
present/release trace. Mesa `ea1cc179` added `ARDESK_WSI_TRACE=1` at successful
presentation and matched release events. XCB resize then passed in
`20260908T213729-0a3e39a5`.

Xlib `20260908T213824-00f8f119` rendered correctly but failed release checking:
a later swapchain allocation reused an old image object's address. Address
based trace identity conflated two allocation lifetimes. Mesa `ae5de449`
assigns a unique allocation ID, stable across that allocation's presents and
releases. It changes diagnostics, not protocol serials or release ordering.
The final XCB/Xlib runs above use this build. Both original failures remain
FAIL in their raw records; no screen or release gate was weakened.

## Remaining scope

Ardesk teapot/scene application workloads are not automatically covered by
these Vulkan clients. Import-path capability parity, delayed release,
disconnect and destruction races still need a complete common gate.
`swapchain-review` remains hybris-only because it uses adapter-specific
allocation hooks. The external display without TAWC-DRI needed for the
missing-protocol case was not exercised in this batch. These results do not
close all WSI or application gaps and do not change Zink's upstream policy.

## Native X window loss — 2026-09-08

The common `--case surface-lost` client acquires an image, transitions it to
PRESENT_SRC and submits its present semaphore. It then destroys the real X
window with a checked request, leaving the Vulkan surface and swapchain alive.
Capability query, acquire, present and per-swapchain result must report
SURFACE_LOST. Failed acquire must leave its index untouched and its fence
unsignaled. After rejected-present queue operations complete, the same binary
semaphore must accept another signal/wait pair with a bounded fence wait.
This shares the preparation and semaphore checks with resize in
`tests/x11/surface_change.c`. The client clears ownership of the destroyed
native window so ordinary cleanup does not destroy it twice.

The failed-present wait requirement also applies to SURFACE_LOST; see
[Khronos vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).
All successful X11 Vulkan cases now require live selected-loader/ICD mappings
and, for validation runs, the actual layer mapping and zero-error verdict.
Previously the host applied these checks only to present/resize; the negative
clients still checked their own validation errors. Missing-protocol now saves
its mappings before teardown, but still requires an external suitable display.

The initial Turnip run `20260908T215458-61dbd032` failed: capability query
reported SURFACE_LOST, but acquire returned SUCCESS with image index 1.
The raw failure is retained. The driver drained configure/release events and
checked connection failure, but TAWC-DRI has no native-window-destroy event.
Mesa `7a6286fc` queries target window geometry before declaring a successful
X11 drain, so an empty event queue cannot make a destroyed window look live.
It also checks the actual extent if configure delivery has not caught up.
This adds X11 round trips; throughput/latency impact has not been measured.

The actual rebuild uses Mesa `7a6286fc337f75c63bb194ba6402566c99283fd7`,
tree `c8b2f23b0204cac816be0e86e8762a0a5e3c3fd1`, Turnip SHA256
`9276b78b402981aecfeed329c96cd9ad5867c082b46da3d3434e44124b873bfc`.
The X11 client and watchdog were rebuilt and their source/binary hashes checked
against the build manifest. Devices, hybris runtime and VVL are as above.
Raw records for this batch are under `/tmp/libhybris-surface-lost-results/`.

| Gate | Hybris / Mali | Product Turnip / Redmi |
| --- | --- | --- |
| XCB surface-lost + validation | `20260908T215458-f2b8ea1d` PASS | `20260908T220104-b06cbe93` PASS |
| Xlib surface-lost + validation | `20260908T215622-70160091` PASS | `20260908T220129-6f0fd2b3` PASS |
| XCB resize + validation | `20260908T215701-933e5728` PASS | `20260908T220200-1f2456a3` PASS |
| Xlib resize + validation | Not rerun in this batch | `20260908T220247-601169e5` PASS |
| XCB acquire-timeout + validation | `20260908T215732-e98ee540` PASS | `20260908T220321-c0b40b15` PASS |

The resize controls retain 24 exact readbacks, six physical screenshots and
two out-of-date/semaphore-reuse transitions each; Mali observes 23 releases,
Turnip 19, for 24 presents without early reuse. Surface-loss checks intentionally
do not claim physical presentation of the destroyed window. Successful runs
have zero validation errors and verified loader, ICD and validation mappings.

This case uses acquire before rejected present; it does not cover present-first
loss discovery, compositor/connection shutdown, delayed release or concurrent
window destruction. No application-level recovery or long-run FD claim is made.
