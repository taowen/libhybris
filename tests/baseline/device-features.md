# Device feature input chains, 2026-09-09

Mali product Zink teapots failed `vkCreateDevice` with
`VK_ERROR_FEATURE_NOT_PRESENT`. Capture `20260909T202515-cfd68abd` shows
`pEnabledFeatures = NULL` and `VkPhysicalDeviceFeatures2.shaderClipDistance = true`.
The compatibility layer advertises its ClipDistance emulation but previously
filtered only the legacy `pEnabledFeatures` request. This report concerns the
GLX/Wayland teapot workload. Blender through Zink is not an acceptance requirement;
Blender uses the direct Vulkan path.

## Change

`layer/device_features.c` now prepares the core feature request in either form.
On a device needing ClipDistance emulation, it copies the chain prefix through
Features2, clears that feature in the copy, and shares the unchanged suffix.
Other payloads and application input memory remain unchanged. Copies use the
application allocator with command scope and are released after device creation,
including allocation and downstream failures. Native ClipDistance devices do not
need a chain copy.

The shallow-copy size switch is generated from the pinned builder's Vulkan
registry, not inferred from memory layout. It covers registered DeviceCreateInfo
extensions available in the compilation headers and the loader's private device
create nodes. To regenerate after a header update:

```sh
podman run --rm --entrypoint cat 62d617eddf37 \
  /usr/share/vulkan/registry/vk.xml > /tmp/vk.xml
python3 tools/generate-device-create-sizes.py /tmp/vk.xml \
  hybris/vulkan/layer/device_create_sizes.inc
```

Registry SHA-256:
`1adbeb17c00be04771aac41bbb33850ff83c3503b42dcb2a25a96b4fd79c7e07`.
Protected enum definitions retain their compilation guard. An unknown node before
the feature needing replacement returns `VK_ERROR_EXTENSION_NOT_PRESENT` with its
sType in the log; it is never silently dropped. Unknown suffix nodes pass through.

The application observer also keeps its selected Vulkan loader and matching glibc
ahead of application libraries, then places the Android ABI frontends last. This
lets product Mesa EGL resolve correctly without mixing the staged loader with a
different libc.

## Actual verification

The AArch64 build and existing headless probe build succeeded. The existing
`icd-caps2` device probe now creates devices with Features2 at both ends of a
five-structure core 1.1 chain and checks every input node remains byte-identical.
It also retains the unadvertised Float64 rejection check. No unit test was added.

| Workload | Mali | Turnip |
| --- | --- | --- |
| `icd-caps2`, head/tail, input preservation, unsupported feature | PASS `20260909T213529-3d89857e` | PASS `20260909T213531-6933ac20` |
| Product GLX + Wayland, present and resize pixels, direct layer deployment | PASS `20260909T213456-12f3e1f0` | PASS `20260909T213457-d40ceb5a` |
| Same product checks after actual APK rebuild/install | PASS `20260909T213804-580ab53e` | PASS `20260909T213804-2fba4231` |

Mali capture after compatibility (`20260909T213021-dc15b46e`) shows
ClipDistance false, successful device creation and 555 presents. The screenshot
was inspected and shows the rendered teapot. Separate post-compatibility VVL
observation `20260909T213155-6e502718` has zero validation errors and ten warnings
about shader output locations without corresponding color attachments. Product
Mali teardown still prints `invalid handle: (nil)`; that diagnostic is not claimed
fixed. Capture replay pixel comparison and full ClipDistance shader semantics are
not established by these checks.

The rebuilt APK is
`6eb20432c7b109342eb19f91c24c94480175e9ae0ff6b48765bfa39c5b301d73`.
Both packaged overlays and both installed layer files match
`69176654433daac7eb7aa14ad8e0d7c888159f44fac696427f39aa804d42f6e1`.
Product checks compare present 560x400 and resized 720x480 frames. The common
runtime and default/explicit layer environment checks also pass.

Local evidence: `/tmp/libhybris-features-results`,
`/tmp/libhybris-unified-app-results`, `/tmp/mali-teapot-{before,after}.jsonl`, and
Ardesk `build/blender-vulkan/device-features/`. The previous failing captures and
product results remain available. Broader gaps, including direct Vulkan Blender's
VS/GS interface error and Turnip's forced BC image failures, remain open.
