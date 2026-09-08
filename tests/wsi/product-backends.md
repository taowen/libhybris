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
