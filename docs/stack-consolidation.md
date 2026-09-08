# 产品栈统一清单（2026-09-08）

本清单按当前源码维护；Mesa 固定
`ae5de4494eb2a4aa8c7adcc116c0aa61bd6efbcb`。
“已统一”只指相应实现/入口已经归并，不表示整个 WSI 或应用验收通过。

## 已统一

| 项 | 现行实现 |
| --- | --- |
| 协议定义 | Ardesk `protocols/` 的 `ardesk-wsi-protocols` 包，包含 TAWC-DRI 0.3 和 `android_wlegl`；hybris 不再自带 XML |
| Xwayland | 只由 Ardesk `third_party/xwayland` 构建；hybris 构建独立客户端，runner 默认附着到已安装、运行中的 `io.taowen.ardesk` |
| 合成器入口 | Ardesk anlabwc scene 消费 AHB；客户端工具不再构建旧测试 APK 或私有 Xwayland |
| 产品 Mesa 源码 | `taowen/mesa` 的 `ardesk-wsi`，固定 `ae5de449`，构建时不 apply WSI patch |
| Mesa 探针构建 | desktop-gl 调用产品 `tools/build/mesa.sh`，直接打包产品库，记录产品提交/源码树/协议校验值；不再维护第二套 pin |
| 窗口验证入口 | `tests/wsi/run.py --backend hybris/turnip` 共用客户端、validation、capture/replay、截图与 X11 release 检查；Turnip 直接复用产品 Mesa runtime，见[证据](../tests/wsi/product-backends.md) |
| frontend 窗口删除 | EGL Wayland/X11 插件和 PRESENT_SOCKET 已删除；Vulkan frontend 插件层已删除，窗口 runner 只走标准 loader + ICD；保留的 Wayland native-window 实现在 `vulkan/icd/` |
| submodule 指针 | 父仓库 gitlink 随已验证、已推送的 libhybris 提交同步；具体版本以 gitlink 为准，指针不代替产品重装 |
| 工具补丁归属 | SPIRV-Tools、GFXReconstruct 的自有修改在各自 fork 中提交；构建器使用完整 commit，记录源码和产物哈希 |

## 仍分开，且应保留两套后端

| 项 | 现状与剩余验收 |
| --- | --- |
| 产品 GPU 后端 | 标准 loader → Turnip WSI（Adreno）；标准 loader → hybris ICD WSI（Mali/vendor HAL）。不需要合成一个 DSO |
| 导入 | Turnip 使用 DMA-BUF 导入/复制路径，ICD 使用 AHB/HAL 查询；都应先验证目标导入路径再广告，不能以普通图像能力代替导入能力 |
| FIFO/release | 两条产品路径都有 FIFO 和实际 release 驱动的复用；跨后端相同负例、断连、延迟 release 和销毁竞态尚无一套完整对照门 |
| usage/alpha/extent | Turnip `wsi_common_ardesk.c` 广告 opaque alpha 和固定 usage 集；ICD `wsi.c` 广告 inherit alpha，并按 HAL 导入查询计算 usage/extent。不能把这些不同值机械改成相同；需以 compositor 实际消费语义和每条导入路径逐项验收 |

## 仍需归并或验收

| 项 | 已确认的剩余分叉 |
| --- | --- |
| 窗口验证/捕获 | 两条 Vulkan 后端已接入共同窗口门；Ardesk teapot/scene 的应用工作负载尚未接入该门。desktop-gl 仍是独立的离屏 Zink 工作负载，不代替应用验收 |
| 历史文档 | 本批给 ICD README、TAWC_FORK、两份 WSI review 和 gaps 加入现行入口/历史范围说明。保留旧运行的真实 APK 名称，不把历史证据改写成使用当前 APK |

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
