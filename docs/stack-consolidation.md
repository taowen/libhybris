# 产品栈统一清单（2026-09-08）

本清单按当前源码维护；Mesa 固定
`bfe5f4ceb762504532ccdd7f19c1c2cdd31791b4`。
“已统一”只指相应实现/入口已经归并，不表示整个 WSI 或应用验收通过。

## 已统一

| 项 | 现行实现 |
| --- | --- |
| 协议定义 | Ardesk `protocols/` 的 `ardesk-wsi-protocols` 包，包含 TAWC-DRI 0.3 和 `android_wlegl`；hybris 不再自带 XML |
| Xwayland | 只由 Ardesk `third_party/xwayland` 构建；hybris 构建独立客户端，runner 默认附着到已安装、运行中的 `io.taowen.ardesk` |
| 合成器入口 | Ardesk anlabwc scene 消费 AHB；客户端工具不再构建旧测试 APK 或私有 Xwayland |
| 产品 Mesa 源码 | `taowen/mesa` 的 `ardesk-wsi`，固定 `bfe5f4ce`，构建时不 apply WSI patch |
| Mesa 探针构建 | desktop-gl 调用产品 `tools/build/mesa.sh`，直接打包产品库，记录产品提交/源码树/协议校验值；不再维护第二套 pin |
| 窗口验证入口 | `tests/wsi/run.py --backend hybris/turnip` 共用客户端、validation、capture/replay、截图与 X11 release 检查；Turnip 直接复用产品 Mesa runtime，见[证据](../tests/wsi/product-backends.md) |
| frontend 窗口删除 | EGL Wayland/X11 插件和 PRESENT_SOCKET 已删除；Vulkan frontend 插件层已删除；Gladio/Vortek GPU host 和 APK 内 shm 演示客户端已删除。窗口 runner 只走标准 loader + ICD；native-window 在 `vulkan/icd/`。旧 overlay 目录 `usr/lib/{gladio,vortek}` 仅在安装时清掉 |
| submodule 指针 | 父仓库 gitlink 随已验证、已推送的 libhybris 提交同步；具体版本以 gitlink 为准，指针不代替产品重装 |
| 工具补丁归属 | SPIRV-Tools、GFXReconstruct 的自有修改在各自 fork 中提交；构建器使用完整 commit，记录源码和产物哈希 |

## 仍分开，且应保留两套后端

| 项 | 现状与剩余验收 |
| --- | --- |
| 产品 GPU 后端 | 标准 loader → Turnip WSI（Adreno）；标准 loader → hybris ICD WSI（Mali/vendor HAL）。不需要合成一个 DSO |
| 导入 | ICD `wsi.c:image_limits` 按 AHB external-image 查询并要求 IMPORTABLE，再筛格式和 usage；Turnip `wsi_common_ardesk_formats.c` 先查询线性 DMA-BUF image 或 DMA-BUF buffer + GPU copy 的导入能力，再筛格式、usage 与 extent；创建时仍验证实际 FD、布局、大小和内存类型。见[查询与真机范围](../tests/wsi/product-backends.md#dma-buf-import-capability-gate--2026-09-08) |
| FIFO/release | 两条产品路径都有 FIFO 和实际 release 驱动的复用；共同门已有 acquire-timeout 和原生 X 窗口销毁后的 surface-lost；断连、延迟 release 和并发销毁竞态仍待验收 |
| usage/alpha/extent | Turnip 保留 opaque alpha，usage/extent 按可导入路径查询计算；ICD `wsi.c` 广告 inherit alpha，并按 HAL 导入查询计算 usage/extent。不能把这些不同值机械改成相同；需以 compositor 实际消费语义和每条导入路径逐项验收 |

## 仍需归并或验收

| 项 | 已确认的剩余分叉 |
| --- | --- |
| 窗口验证/捕获 | 两条 Vulkan 后端已接入共同窗口门。原生 scene 与产品 teapot 复用同一 Host（`tests/device_gate.py`、`Host.deploy`/`pull_maps`）。已删除 `tests/test-wsi-buffer-layout-device.sh` 这条 guest-desk + `TEAPOT_ONCE` 旁路。VVL/capture 仍只挂在 `tests/wsi` 探针上。desktop-gl 仍是离屏，不代替应用窗口验收 |
| 历史文档 | `TAWC_FORK.md` 已把已删 Vulkan/EGL 窗口插件及旧打包入口改为退役说明；`integration-review.md` 的现行入口与旧夹具记录分段。旧 APK/hash/run ID 保持原样，不作为当前产品门的证据 |

## 构建边界与 Mali quirk

独立 libhybris checkout 通过 `ARDESK_WSI_PROTOCOL_DIR` 指向共享协议包；
嵌入 Ardesk 时构建器默认查找 `../../protocols`。产品打包/安装通过
`HYBRIS_LIB_DIR` 选择已构建的 hybris 安装产物，不能再复制一份协议 XML。

MMUD 是 HAL 初始化兼容处理，既不是协议字段，也不是 WSI 能力。
当前构建启用 `--enable-mali-quirks` 后，对
`libGLES_mali.so` build-id `5ac4efe8d6175298b273dbaeb8f9d28e5e508e72`
自动启用进程内 hook；`HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK=0` 可关闭，
`1` 显式启用。其他 build-id 和 secure execution 不走此 hook。
因此应区分“构建选择启用 Mali quirks”和“运行时默认自动匹配”，不能再
把当前行为写成运行时纯 opt-in。详见[初始化证据](mali-mmud.md)。

## 逐项验收边界

当前共同窗口用例已验证两条后端的 FIFO 呈现、实际 release 后复用、
零/有限 acquire 超时、resize 和原生 X 窗口销毁后的 surface lost。它们
未验证延迟 release、连接断开或并发销毁，也没有据此关闭全部契约。

现有颜色帧 alpha 为 1，不能证明 opaque/inherit 下非平凡 alpha 的
合成行为。usage 也需按每个广告位的真实图像操作验证，而不是仅在
createInfo 中请求该位。extent 的 resize 证据不涵盖所有边界和创建竞态。

Ardesk teapot 的 Zink 真窗口与 `tests/test-scene-ahb-device.py` 的原生
AHB scene 是不同工作负载，但共用 Host。scene 不是 Vulkan 客户端，不能
靠设置 VVL/GFXReconstruct 环境冒充窗口探针。teapot 的画面和 resize
由产品像素检查负责；Khronos 层与 GFXReconstruct 仍以 `tests/wsi` 的
Vulkan 探针为准，不为应用再复制一套 capture 比较路径。
`desktop-gl` 的离屏结果仍不能代替任一应用级窗口门。

延迟 release、连接断开、并发销毁、advertised usage 的真实操作、
alpha≠1 以及无 TAWC-DRI 的 missing-protocol 仍是扩覆盖，不是把现有
入口再合成一套。`swapchain-review` 继续只走 hybris allocation hook。
