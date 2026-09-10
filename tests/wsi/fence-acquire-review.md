# Fence acquisition cleanup review — 2026-09-10

The uncommitted `init-layout` diagnostic transitioned every swapchain image
before acquiring any image. On the previously committed Mali driver it
completed, but Vulkan validation reported three
`UNASSIGNED-non-acquired-swapchain-image-used` errors (baseline run
`20260910T113134-09e790a8`). This violates the
[Vulkan image acquisition contract](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html#_wsi_swapchain).

The draft's eager HAL image acquisition, hidden queue submissions, global
fence completion cache and unconditional shader/device tracing were discarded.
The cache could falsely mark all fences complete after a wait-any operation
and did not identify fences by device. No production synchronization behavior
change remains; the swapchain source change only corrects its resize comment.

The replacement `fence-acquire` test requests color-attachment plus transfer
usage and acquires each image before submitting its transitions. Each of eight
frames checks a bounded fence wait, signaled status, a repeated zero-timeout
wait, reset and unsignaled status. It then renders and checks GPU readback,
physical screenshots and native release before reuse. Existing semaphore
presentation and lifecycle tests remain complementary coverage. The test
requests color-attachment usage but renders with transfers, so it does not
claim coverage of a color-attachment render pass.

X11 tests now require OPAQUE composite alpha, matching the Wayland probe,
and log requested usage instead of advertised usage. UNSUPPORTED is not PASS.
Resize documentation now distinguishes queued SUBOPTIMAL presentation from
an exhausted stale pool returning OUT_OF_DATE and sticky surface loss.

## Reproduction and artifacts

From the libhybris checkout, build with:

```sh
python3 tests/x11/build.py
tools/build-aarch64.sh --headers /tmp/libhybris-wsi-resize-build/headers --out /tmp/libhybris-cleanup-build
```

Run each matrix entry using this template (select platform and case):

```sh
python3 tests/wsi/run.py --serial 10AFA31610002QH \
  --backend hybris --platform xcb --case fence-acquire \
  --build /tmp/libhybris-cleanup-build \
  --icd-hal /vendor/lib64/hw/vulkan.mali.so --icd-mali-loader-quirk \
  --vulkan-loader tests/desktop-gl/build/runtime/libvulkan.so.1 \
  --validation-layer /tmp/libhybris-validation-current/install/lib/libVkLayer_khronos_validation.so \
  --validation-manifest /tmp/libhybris-validation-current/install/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
  --out /tmp/libhybris-cleanup-results/mali
```

For Redmi use serial `29854870`, output directory `adreno`, HAL
`/vendor/lib64/hw/vulkan.adreno.so`, and omit the Mali quirk. For Turnip use
`--backend turnip` and omit both HAL and loader arguments; the runner verifies
and stages the product Mesa runtime from `tests/desktop-gl/build`.
An Ardesk X display must be running. The first Redmi attempt
`20260910T113533-2b5d3cef` failed X11 connection before Vulkan initialization;
the display was started before the recorded reruns below.

Both builds succeeded. Artifacts were built from `ed5b49c37e2d` plus this
cleanup; per-run JSON records include source, runtime and driver hashes,
validation-layer mappings, compositor identity and screenshot verdicts.
Raw local evidence is under `/tmp/libhybris-cleanup-results/{mali,adreno}`.
Screenshots and large staging manifests are not checked into the repository.

| Artifact | SHA-256 |
| --- | --- |
| Hybris ICD | `cbc3bd31416a2f58515e0329e3d677722acb1d91dd34165145b240c838f6b253` |
| X11 probe | `91fe243a393ed0e74ce7a464f5689c010a9343ebfde7031098e91c3644fea70d` |
| Validation layer | `6b085cc9058769fde84f88870d64fa106f863f70e32e2d2baf16f87c785fbc7e` |

## Device results

| Device | Backend | API | Case | Result | Run ID |
| --- | --- | --- | --- | --- | --- |
| Redmi | hybris | xcb | fence-acquire | UNSUPPORTED | `20260910T113831-ad78152f` |
| Redmi | hybris | xlib | fence-acquire | UNSUPPORTED | `20260910T113904-b085c67f` |
| Redmi | turnip | xcb | fence-acquire | PASS | `20260910T113944-05f00a05` |
| Redmi | turnip | xlib | fence-acquire | PASS | `20260910T113955-2a289cfc` |
| Redmi | turnip | xcb | present | PASS | `20260910T114006-4abef07f` |
| Redmi | turnip | xcb | resize | PASS | `20260910T114017-ad626ded` |
| X300 | hybris | xcb | fence-acquire | PASS | `20260910T113532-483e9e01` |
| X300 | hybris | xlib | fence-acquire | PASS | `20260910T113541-861c6bf1` |
| X300 | hybris | xcb | present | PASS | `20260910T113550-72584223` |
| X300 | hybris | xcb | resize | PASS | `20260910T113559-b9062779` |
| X300 | hybris | xcb | acquire-timeout | PASS | `20260910T113619-cfb6f4a8` |
| X300 | hybris | xcb | surface-lost | PASS | `20260910T113623-a67355f7` |

All ten PASS runs had zero validation messages, including synchronization
validation, and passed the applicable readback, screenshot, buffer-release
and compositor isolation checks. Redmi native Adreno advertised only
INHERIT (`0x8`), so its two fence cases stopped before swapchain creation;
native Adreno fence synchronization is not covered by those runs.
