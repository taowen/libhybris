# libhybris 图形兼容性与渲染诊断缺口

调研日期：2026-09-06。当前 libhybris 基准：`dcc3588262a2b5181a15998c63d5e7e684ef2631`。
范围：评估把本仓库扩展为同进程的 GLES / 桌面 OpenGL / Vulkan 兼容栈；借鉴 Vortek、Gladio 的能力，不照搬它们的命令 IPC。
本文最初为调研建议，现作为持续实施的验收清单；阶段进度见 [实施状态](IMPLEMENTATION.md)。下文目标结构和完整兼容层尚未完成，两次 Blender 故障尚未重新复现。

## 当前产品栈与统一状态（2026-09-08）

现行 compositor 入口为已安装的 `io.taowen.ardesk`；Xwayland 由 Ardesk
构建，libhybris 只构建客户端。后文 `io.taowen.hybriswsitest` 等名称属于
注明日期的历史记录，不能作为当前夹具或验收步骤。
产品 Mesa 已使用 `taowen/mesa` 的 WSI fork `bfe5f4ce`；下面的官方
`c3b008c1` 仅为历史离屏证据；desktop-gl 已直接复用产品 Mesa 构建和库。
frontend Wayland/X11 窗口插件、Vulkan 插件加载层及 PRESENT_SOCKET 已删除，
ICD 所需的 native-window 实现移至 `vulkan/icd/`。
旧 desktop-gl 无验证错误声明已被独立日志复核更正，不能沿用作验证通过。
两条产品 Vulkan 后端已共用窗口探针、标准 layer/capture 和屏幕证据门，
见[共同窗口证据](tests/wsi/product-backends.md)；应用及故障竞态门仍需验收。
已统一项与剩余分叉见[产品栈清单](docs/stack-consolidation.md)。

## 官方 Mesa 切换时的历史记录（2026-09-07）

当时 Mesa 改为官方 `c3b008c1`，不再依赖 Mesa fork、Gallium Freedreno
KGSL 或定制 Zink 顶点模拟。此前 fork 的通过记录保留为历史，不能视为当前
官方实现的覆盖。原版 Turnip + Zink 在 Redmi 的 EGL/GLX 两轮通过 38 张一致
图像及 44 份 SPIR-V 验证，SSBO 为 16/16/16，SyncVal 零错误。Mali 官方 Zink
拒绝 core 3.3；core 3.2 的 packed attribute 用例失败。Mali Blender 显示
需要 OpenGL 4.3 的对话框，启动脚本未执行；Redmi 未安装 Blender。详见
[当前官方 Mesa 证据](tests/desktop-gl/README.md#official-upstream-results-2026-09-07)。
后续工作沿用官方 Mesa。按用户确认，Zink 不兼容 Blender 可以接受，
Blender 经 Zink 的启动/渲染不再作为验收条件；上述失败保留为观察记录。
其余适用的驱动能力、标准接口和回归项继续验收，不能通过恢复私有 Mesa
补丁或提高宣告值来记作完成。G07/G08/G13 等整项保持开放。G10 已关闭：
产品桌面 GL 就是 Zink。

## 1. 结论与边界

当前 libhybris 已能把 glibc 程序接到 Android GPU 驱动，但它还不是 Vortek 式 Vulkan 语义兼容层。桌面 GL 产品 frontend 是 Mesa/Zink（G10 已关闭），不是 Gladio。基础测试通过与复杂应用正确渲染之间，主要缺少：

1. 完整、可验证、不会绕过包装的 API 分发与对象管理。
2. 任意应用级 validation/capture 覆盖。标准 loader 的 headless widget 与窗口探针已接 VVL 和 GFXReconstruct，加载链不是未接线。
3. 从一次 draw/submit 追到实际 descriptor、内存内容、attachment 和最终呈现缓冲区的证据链。
4. 有真实能力约束、有像素回归测试的格式/着色器/同步兼容处理。
5. 应用级窗口（teapot/scene）与 Vulkan 探针仍是不同工作负载；故障竞态和 advertised usage 的真实操作尚未用共同门打完。

窗口提交本身已经不是缺口：产品路径是 Zink + 标准 loader → ICD/Turnip WSI → TAWC-DRI / android_wlegl → anlabwc。不要再做第二套跨进程 present 或 hybris EGL 桌面窗口插件。

建议大幅改造 **GPU API 层、兼容层和诊断层**，保留已有 Android loader、libc hooks、TLS 修补作为底座，先用回归测试保护它们。重写 TLS 和图形兼容层同时进行，会让故障来源难以区分。

“尽可能兼容”应以正确运行的应用和已验证语义衡量，不以谎报 Vulkan/GL 版本、扩展数量或创建窗口成功衡量。模拟有性能成本；硬件不足且没有正确实现时，应明确报告不支持。

## 2. 已掌握的证据

### 2.1 29854870 独立真机基线

使用本仓库 [tests/baseline](tests/baseline/README.md)，无 APK、rootfs、Xwayland 或合成器。相同 C 程序分别编译为 bionic 和 glibc，以 Android shell UID 运行；不能据此证明 Android app UID 下的权限/命名空间行为相同。

| 项目 | Android 原生 | glibc + hybris |
|---|---|---|
| EGL 初始化 | 1.5 | 1.5 |
| GLES 2 / GLES 3 context 请求 | 都成功；实际返回 GLES 3.2 | 同左 |
| 离屏 clear、shader 编译/link、三角形像素读回 | 通过 | 通过 |
| Vulkan instance/device、buffer fill、fence、1024 words 读回 | 通过 | 通过 |
| Vulkan 扩展枚举 | 与 hybris 相同 | 与原生相同；使用 null WSI |
| EGL 桌面 OpenGL context | 不支持 | 不支持 |

设备型号 `M2012K11AC`，Android 13 / SDK 33；驱动自报 Adreno 650、Vulkan 1.1.128、driver `0x801f6000`、GLES 3.2 V@0502.0。以查询结果为准，不根据机型推断 GPU。
已查询：BC=false、ETC2/ASTC=true、geometry/tessellation=true、float64/int64=false。这些只是 capability，未验证对应绘制。
此次扩展清单没有 timeline semaphore、dynamic rendering、synchronization2、descriptor indexing、device fault；它不能代表另外两台 Blender 故障设备。

局限：Vulkan workload 请求 1.0；现已补 transfer、widget graphics UBO 及双线程 device/fence 生命周期，未覆盖 compute shader、texture、AHB、WSI 或 CTS。不能称为“Vulkan 1.1 已通过测试”或“GLES 3.2 全兼容”。
记录在 [初始基线证据](tests/baseline/smoke-results.md)；原始 JSON/log 留在本地被忽略的 `tests/baseline/build/results/`。本轮未重跑完整应用。

### 2.2 初始源码摸底，而非旧安装产物

本表是文首 `dcc3588` 的初始源码快照，不是当前 HEAD 的功能清单。
后续实现及其验证边界见 [实施状态](IMPLEMENTATION.md)。例如当前
`hybris/vulkan/icd/instance.c` 与 `device.c` 已分别保存 instance/device
的 resolver、generation 和 allocator，并包装创建/销毁及物理设备枚举；
这些对象记录不等于所有命令已有兼容状态或完整语义覆盖，G02/G03 仍开放。
HEAD 已删除 `hybris/vulkan/platforms/`；窗口 WSI 在 `vulkan/icd/`。
`vulkanplatform_x11.so` 只是当时安装残留，不是快照源码能力，也不是 HEAD 源码能力。

| 模块 | 当前证据 | 不能据此推断 |
|---|---|---|
| [Vulkan 包装](hybris/vulkan/vulkan.c) / [导出](hybris/vulkan/vulkan_exports.c) | 独立文件持有导出 trampoline，GIPA/GDPA 对少数 WSI 函数拦截，其余转发 | 所有新加兼容处理都会覆盖静态链接、dlsym、GIPA、GDPA 四种入口 |
| Vulkan 平台构建（`dcc3588` 快照路径 `hybris/vulkan/platforms/Makefile.am`，HEAD 无此文件） | 快照时 common/null/wayland；Xlib/XCB surface 在 Vulkan 包装中返回不支持 | 当时安装残留的 `vulkanplatform_x11.so` 是当时或现在的源码能力 |
| [GLES 包装](hybris/glesv2/glesv2.c) | GLES 导出与 Android 库桥接 | 提供桌面 OpenGL core/compat、GLX 或完整 GL→GLES 转换 |
| EGL 桌面窗口插件 | 已删除；`ws_init` 拒绝 wayland/x11 | 不能再把 hybris EGL 当前窗口当产品路径 |
| ICD Wayland/X11 WSI | `vulkan/icd/` 经 android_wlegl / TAWC-DRI 提交 | 已通过共同窗口门；故障竞态仍开放 |
| [loader bridge](hybris/common/linker_bridge.c)、[libc hooks](hybris/common/hooks.c)、[同步桥接](hybris/common/bionic_sync.c)、[TLS 说明](TAWC_FORK.md) | 独立 Android linker、libc/线程桥接、ARM64 TLS thunk | 任意 Android 版本、任意 vendor library 都兼容 |

摸底中发现本机旧安装目录残留 `vulkanplatform_x11.so`，但当前源码没有相应构建目标。以后必须干净 staging，并记录实际加载文件的 build-id/SHA256，不能拿安装目录文件名证明源码功能。

### 2.3 Vortek / Gladio 能借鉴什么

Gladio/Vortek 的跨进程 command IPC、PRESENT_SOCKET 和 APK 内 GPU host
已经从产品树删除。现行接入是同进程：Zink + 标准 loader → hybris ICD 或
Turnip，窗口经 TAWC-DRI / android_wlegl 交给 anlabwc。历史参考源码在
工作区 `x11-glibc-apk`，见 [源码范围核对](docs/vortek-scope.md)；那是算法
对照，不是产品路径，也不是 CTS oracle。

| 参考 | 观察到的能力 | 产品现状 |
|---|---|---|
| Vortek ShaderInspector | scaled vertex / SPIR-V 条件处理 | 同进程 ICD `compat/`，无 RPC object |
| Vortek TextureDecoder | BC 解码与替代 upload | 同进程 ICD `compat/` |
| timeline 等待传输 | 经 eventfd 的 RPC 等待 | 未移植；原生 core/KHR timeline 透传 |
| Gladio 宿主 / 命令环 | GLES 命令 IPC | 已删除；桌面 GL 走 Zink |

Vortek 的封包不能改善 API 语义。不要把 command ring/socket 加回来。

## 3. 两次 Blender 失败揭示的缺口

下面引用的 `../../docs/` 是父级 ardesk 的本地调查记录，独立 clone 本仓库时不可访问，因此这里保留必要摘要；不是本轮重新验证的结果。

### 3.1 Mali：立方体正常，widget 白底、品红条

来源：[BLENDER_HYBRIS_MALI_WIDGET_UI.md](../../docs/BLENDER_HYBRIS_MALI_WIDGET_UI.md)。设备 `10AFA31610002QH` / Mali-G1-Ultra，Blender 4.3.2，历史实验 Vulkan X11 路径；兼容转换和 X11 WSI 后来全部回滚。

记录已观察：

- 80/96 字节 push constants 的立方体可画；widget std140 UBO 272 字节，超过设备 256 字节 push constant 限制；instanced widget 约 1.2KB，更不可能靠扩大广告值解决。
- widget UBO layout：parameters@0、MVP@192、checkerColorAndSize@256、srgbTarget@268。复制/alias 实验已读回真实矩形与矩阵数据，不能继续只假设“UBO 一直是零”。
- 但在 `vkUpdateDescriptorSets` 看到正确数据，不证明 draw 实际使用同一个 set/buffer/range/layout，也不证明目标 attachment 被正确合成。
- 品红条被记录为错误纹理路径，应与 widget UBO 问题分开验证。
- `vkCmdBeginRenderingKHR` 包装曾解析到错误 trampoline/空操作；修改 indexed draw 的实验也有真实函数指针未确认的疑点。
- 非法删除 clip capability/member 曾引发 GPU fault；对带 UBO shader 跳过处理没有改变图像。截图相同是一次实验结果，不是所有条件下的数学排除证明。

必须补的复现与观测：

1. 272-byte UBO 和约 1.2KB UBO，显式核对 bool/int、mat4 stride、descriptor dynamic offset；shader 把选定字段编码到像素/SSBO，证明 **shader 读到** 正确值。
2. 12 vertices / 18 indices、`gl_VertexIndex` widget，依次验证直接 UBO、staging copy、descriptor update/template、重录与重提交流程；每步有预期像素。
3. 每个 draw 记录有效 pipeline/layout、set binding、buffer generation、offset/range、push constants、render target view/subresource；不要只有 UpdateDescriptorSets 日志。
4. 分别读回 widget region FBO、合成后 image、送出的 AHB；找出第一处像素分叉。此调试读回只能按需启用，并保持正确同步。
5. shader 原始/转换后 SPIR-V、反射结果和缓存 key 可关联到 pipeline；真实 backend 函数解析也要可查询。

尚不能确定根因是 descriptor 绑定、shader 编译、resource lifetime、UI FBO/blit，还是几者组合；不应在新实现中预置“UBO 修复”作为结论。

### 3.2 Adreno / Turnip：View3D 重绘后 device lost

来源：[BLENDER_TURNIP_DEVICE_LOST_INVESTIGATION.md](../../docs/BLENDER_TURNIP_DEVICE_LOST_INVESTIGATION.md)。OnePlus PJZ110 / Adreno 830v1，Turnip 26.3.0-devel，Blender 4.3.2 Vulkan。
**这不是 hybris/vendor 驱动失败的证据**；它提供真实 workload 与诊断方法的回归需求。

记录显示：基础 readback、首次绘制以及 View3D-shaped probe 都可通过；orbit/redraw 后可能 lost。复制“大尺寸 RGBA16F + D24S8 + 多次 LOAD/copy/blit”仍不能复现。
尚未复制的组合包含 bindless descriptor、多个纹理访问、较复杂 shader、packed SNORM 与 instanced vertex input、连续 attachment/resource 复用。RD/cffdump 中 prefetch/constlen 是 Turnip 编译结果，不应变成跨厂商 Vulkan 测试的硬条件，也不能从 GPU 的 bindless 位直接推定应用用了某个 descriptor indexing feature。

下一步应从合法 Vulkan source probe 增量复刻上述组合：保留每一步 diff、输入和像素/崩溃结果。API replay 用于缩小范围，RD/cffdump 仅为 Turnip 可选证据，不手工裁剪 GPU packet 作为最终测试。
`vkWaitForFences` 报错只是检测点，不必是触发点；需要关联最近提交、实际执行完成序号和最后已知正确 attachment。

## 4. 建议的目标结构

```text
桌面 GL 应用 ─→ Mesa Zink ─┐
                           ├→ 标准 loader → hybris ICD 或 Turnip
Vulkan 应用 ───────────────┘         │
                                     ├→ Wayland：android_wlegl → anlabwc → Surface
                                     └→ X11：TAWC-DRI → Xwayland → android_wlegl → anlabwc
GLES 应用 ─→ hybris GLES（非产品窗口路径）
```

API 兼容处理不认识窗口 XID/合成器私有 socket；WSI 不修改 shader 或伪装 GPU feature。AHB 是可共享缓冲区，不是命令协议。
Wayland 的 `android_wlegl` 和 X11 的 `TAWC-DRI` 都是双方必须实现的扩展，不是核心协议自动支持 AHB。标准 DRI3/dma-buf 适配需要另行证明格式、modifier、同步与句柄兼容，不能把 AHB fd 直接等同 dma-buf。

### 4.1 Vulkan loader / ICD 决策必须先验证

长期优先评估：保留标准 glibc Vulkan loader，新增 hybris ICD adapter，compatibility 作为可组合模块/layer，使标准 validation 和 capture 有正常入口。ICD 需实现协商、dispatchable object、physical-device proc 查询和 surface/WSI 约定，不能只给现在的 `libvulkan.so` 写一份 JSON。[Khronos loader-driver 接口](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)

已验证 headless 初步路径：标准 glibc loader → 可选 hybris ICD → vendor Vulkan HAL，在两台 Adreno 650 设备通过 8 项基础探针。标准 validation 及离屏 widget capture/replay 已验证；标准 ICD 窗口路径的 VVL 与 GFXReconstruct 虚拟 swapchain 捕获/回放已在一加 8T 和 Mali 通过，仍非任意应用或完整 WSI 符合性。Android loader 已在 bridge 下游，必须验证双 loader 的 handle/dispatch 所有权，防止递归加载同名 libvulkan；Android 库用独立路径/命名空间明确解析。
短期可先整理当前 frontend 的统一 dispatch，再做 adapter spike。若继续以替代 `libvulkan.so` 方式交付，也必须实测 layer chaining；`VK_LAYER_PATH` 不是自动接入的保证。

compat 在标准 layer 里时，验证原始 app 调用和转换后 backend 调用需要分别安排验证边界；位于 ICD 内的内部转换不会自然被上游 layer 看到。glibc layer 不能直接丢给 bionic loader 加载。

### 4.2 OpenGL 路线选择

2026-09-07 路线决定：桌面 GL 以 Mesa/Zink 为主线，复用 Mesa 的 GL
状态机、GLSL 和 core/compat frontend；这是维护与覆盖方向的选择，尚无
本项目同应用对照证明 Zink 全面优于 Gladio，也不意味着 Blender 已通过。
Adreno 在所固定 Turnip 支持且实测通过的设备上采用 Zink → Turnip；
Mali 等设备采用 Zink → hybris Vulkan → 厂商驱动，并按实际缺口补兼容。
高通也保留 Zink → hybris Vulkan → 原厂驱动这条选择；libhybris 并不限于
Mali。选择原厂驱动还是独立 Turnip 后端，应根据设备实测能力和工作负载
决定。某个原厂驱动不支持 timeline 不代表 libhybris 不支持该 GPU 厂商。
统一的是 GL frontend 和 Vulkan 接口，不能把 Turnip 当成跨 GPU 后端。
Gladio 的有效应用经验继续作为回归参考，GLES 路径保留给现有用例及
Vulkan 不满足要求的设备，不同时重建另一套通用桌面 GL 前端。

当前 Adreno 构建已退掉 Gallium Freedreno 直接 GL 后端。
父项目 `tools/build/mesa.sh` 只选择 Gallium `zink` 和 Vulkan `freedreno`
（Turnip）；`freedreno-kmds=kgsl` 是 Turnip 的内核接口选项。
Turnip 仍使用 Freedreno 共享编译器和设备代码。官方版本已通过的离屏
EGL/GLX 用例不代表真实窗口呈现、resize 或同应用性能对照已完成。
Zink 的厂商能力模拟也必须以真实绘制及 validation 为准，不能只提高
GL 版本或资源数量宣告来绕过应用检查。

| 路线 | 价值 | 明确限制 |
|---|---|---|
| Mesa/Zink → hybris Vulkan | 优先评估现代桌面 GL，复用完整 GL 状态机/编译栈 | 取决于所固定 Mesa 版本的 Vulkan feature/format/limit 要求，不能仅看 Vulkan 版本号 |
| GL→GLES 独立前端，借鉴 Gladio | 旧 GL/兼容 profile、厂商 Vulkan 能力不足的设备 | 固定管线、GLSL、texture/FBO、GLX/context 语义工作量大，不承诺小改动支持现代 Blender |
| GL4ES | 可比较的旧 GL 参考实现 | 目标 GL 1.5/2.1，不是现代桌面 GL 的完整替代 |
| ANGLE | GLES 实现与 shader/driver workaround 的参考/可选层 | 不是桌面 GL 实现，也不能凭空补全底层 Vulkan 功能 |

固定 Mesa commit 后，用 Zink requirements/profile 检查器得出按 GL 目标版本的差集；最新文档已包含多个扩展要求，不采用“Vulkan 1.3+ 必然够用”的经验判断。[Zink](https://docs.mesa3d.org/drivers/zink.html)、[GL4ES](https://github.com/ptitSeb/gl4es)、[ANGLE](https://github.com/google/angle)

29854870 的 GLES 基础路径可作为 GL→GLES 实验底座；其 Android
厂商 Vulkan 驱动缺少多项现代扩展。另选官方 Turnip 时，当前 Zink
EGL/GLX 探针均报告 GL 4.6，并通过文首列出的 38 张图像对照；这是
指定驱动与工作负载的覆盖，不是 GL 4.6 全面兼容或 CTS 通过的证明。

同日 baseline 的 native-3 / hybris-3 在三台设备均 PASS，GLES 能力
查询逐项一致且 GL error=0。X300（`20260907T131453-7e930597`）顶点
SSBO/图像上限为 0/0，片元和计算 SSBO 均为 35；红米 29854870
（`20260907T131453-2abee58b`）与 OnePlus KB2000
（`20260907T131453-7a91c77a`）顶点 SSBO/图像均为 4/4，片元 SSBO
为 4，计算 SSBO 为 24。这说明仅直通 GLES 不能满足当前 Blender
检查的每阶段 12 个 SSBO；不证明 Gladio 的实际转换无法运行其他应用，
也不构成三台设备的桌面 GL、完整 GLES 或性能验收。

## 5. Gap 清单与验收条件

P0 = 兼容增强前的基础；P1 = 直接影响目标应用；P2 = 基础可用后的扩大覆盖。以下是待做项，不是已实现功能。

| ID / 优先级 | 缺口与风险 | 最小验收证据 |
|---|---|---|
| G01 / P0 | 自主构建、固定输入与实际产物来源 | 独立 checkout 构建脚本，固定 headers/compiler/deps；运行 manifest 含实际 ELF hash/build-id、driver、设备、env、quirk 配置；干净安装无旧平台库。**AArch64 baseline 已验证**：本仓库固定 base digest / Debian snapshot 和 Android headers commit；构建前源码/headers 快照哈希、容器 ID、包版本、编译器及配置均留档。探针有独立 manifest，运行记录命令、driver 字符串与分阶段 mappings，关联部署哈希并对 Android 路径事后取哈希。独立 checkout 在两台设备均 21 PASS / 2 UNSUPPORTED；详见 [构建证据](tests/baseline/build.md)。映射快照不等于全生命周期追踪或内存页校验 |
| G02 / P0 | 全入口 dispatch、core/KHR alias、每 instance/device 的真实函数表；避免包装绕过/递归/NULL branch | 从 vk.xml 固定版本生成覆盖表；同一测试经 link/dlsym/GIPA/GDPA；BeginRendering/KHR、Submit2/KHR 等按启用能力测试；未支持符号符合规范，不假成功。**部分落地**：普通 GIPA/GDPA 查询保留 backend 对 instance/device 的解析结果，只替换必要前端/WSI 包装；新增作用域/未启用扩展的负例测试，未解析的直接导出调用有错误信息。已从固定 Vulkan-Headers v1.4.309 registry 生成 726 个命令的 scope/alias/provider 清单；真机记录 dlsym/GIPA(NULL)/GIPA(instance)/GDPA，检查 137 个 core 1.0 必需入口及非 global/non-device 的禁止作用域。相同 fill/fence/readback 已经 link/dlsym/GIPA/GDPA 实际执行，memory2 的 core 1.1/KHR 已启用并验证。swapchain 包装已按当前 device 的 GDPA 解析并在缺失时先拒绝，Wayland 创建/销毁按当前 instance 查询（仅构建验证）；未启用扩展的直接导出负例不再返回成功。可选标准 ICD 已保存 instance/device resolver/destructor 与各自唯一 generation，物理设备普通枚举及 core/KHR group 枚举登记所属 instance，并由创建/查询/销毁入口使用；替代前端已登记 device/queue/pool/command buffer 归属，并按实际 device 的 GDPA 分发四组 core/KHR 渲染命令；X300 的固定绘制在四条前端入口均通过（详见文末）。timeline 三组 core/KHR 主机命令也已按传入 device 解析并实际执行。前端完整 resource/generation 状态、全部 alias、完整渲染/同步语义仍未覆盖 |
| G03 / P0 | 多线程/多 context/多 device 与对象生命周期 | 并行 create/destroy、二次 init/dlopen、回调、线程 TLS、handle 重用有回归；对象 state 按 generation 识别，不能用进程全局单一 current device。**部分落地**：谁持有谁：glibc 持有 `libhybris-common`（`DF_1_NODELETE`）至进程结束；common 持有 Android linker plugin，不 `dlclose` 它；frontend 持有自身 glibc 引用；vendor 对象由 `android_dlopen` 调用方持有，frontend 关闭不回收它们。允许的关闭顺序：销毁 Vulkan device/instance，丢掉 frontend 引用，进程退出。hook 表首次 qsort 已改为 pthread_once 后发布，避免无锁 sorted 标志导致排序/搜索竞争；现有并发初始化回归通过，但内部 lookup 未导出，未独立动态复现首次排序竞争。callback 发布与查询快照、缺失符号编号已改为原子操作，未命中日志开关改为 pthread_once；公开接口注明 callback 替换不等待旧调用结束，生命周期仍由调用方保证。这些共享状态修复仅有代码审查和现有回归证据，未独立复现 callback 替换竞争。`ENSURE_LINKER_IS_LOADED()` 改为 `pthread_once`：审查现有 bundled linker 初始化路径未发现重入公开 `android_*` wrapper，后续新增回调必须继续遵守该约束；`_hybris_hook_dlerror` 改为直接调 `_android_dlerror`，避免 once 期间重入。baseline `life` 覆盖双 device、destroy/recreate、二次 dlopen、两线程 create/destroy，但不覆盖首次进入。`init` 覆盖两线程同时第一次 `android_dlopen`；`tls` 覆盖工作线程完成 Vulkan 创建/销毁后主线程关闭 frontend、再让工作线程退出；已用 bionic C++ DSO 的 emulated TLS 和原生 TLSDESC 两种产物观测初值与析构；原生变体曾在 glibc 工作线程首次读到 0，现由 Q linker 静态 TLSDESC resolver 先初始化线程并重放 .tdata 修复。关闭主引用后，析构在所属线程恰好执行一次、值正确，三轮均通过；不证明 compat allocation 清理、IE TLS/signal 场景或静态槽回收。`20260906T233143-5f5b13ed`：hybris `init`/`tls`/`unload` PASS；native `init` 无对应实现，不再调度；hybris common 加载失败报 FAIL。注入第二次 pthread_create 失败后能释放 barrier、join 已启动线程并退出 2。`dlclose` 返回 0 不等于已解除映射；进程退出 0 不等于资源已回收；重复创建成功不等于 generation 管理。可选 ICD 新增 instance generation 表，四线程 16 次创建/销毁记录在两台设备配对且无遗留；探针进一步要求每轮四个 instance 同时存活；另有应用 allocator 三轮创建/查询/销毁无未释放分配、拒绝分配返回 OUT_OF_HOST_MEMORY 的三路径对照；标准路径失败可能在 loader 层发生；另有直接 ICD 探针验证状态记录首笔分配失败仅调用一次 allocator、恢复后三轮生命周期无遗留，仍不代表每个 HAL 分配点或 allocator 内部锁交互；分配回调按规范不得调用 Vulkan 命令。上述旧证据仅覆盖 instance；新 device 表在 life 的 19 次生命周期中核对 generation、父 instance、销毁配对及零遗留，观测到原始 device handle 复用。直接 ICD allocator 探针覆盖普通/core/KHR group 三种枚举后的 device 状态首笔分配拒绝及恢复；新增直接 ICD 普通/core/KHR group 枚举的回调分配拒绝及同 instance 恢复，两台设备三路径均返回 OUT_OF_HOST_MEMORY、恢复后 device 创建/销毁成功且每轮回调分配无遗留；拒绝覆盖 adapter/HAL 边界，未逐 HAL 分配点隔离。resource 状态、部分多 GPU 枚举失败及并发枚举仍未覆盖。已补 GLES 两个不共享 context 的 buffer/clear state 隔离、pbuffer 精确像素、主线程→工作线程→主线程迁移和三轮销毁重建，两台设备 native/hybris 均通过；补充 `vk-init` 四线程同时首次枚举/创建并各自完成四轮 instance 查询/销毁，平台初始化与全局入口解析改为 pthread_once；null 平台真机验证，Wayland 仅构建验证。静态 Android mutex 改为受锁保护的唯一后端指针发布，兼容四字节对齐；bionic DSO 的 32 把新锁、四线程首次竞争在两台设备通过，旧 common 负对照超时。已补 rwlock 唯一后端发布，32 把新锁四线程写互斥及同时读持锁/try-write 拒绝；cond 的首次指针发布也已串行化，并补 32 个新条件变量的 timedwait/signal/broadcast 正常唤醒探针；旧版本次亦通过，未复现丢失唤醒。缺少 glibc /dev/shm 时 allocator/translation 不再空指针崩溃，三种共享同步对象初始化返回 ENOMEM；这不代表正常共享同步已支持。两个 monotonic cond 别名已修复为显式 CLOCK_MONOTONIC，100ms deadline 不再立即超时；relative 入口改为 monotonic 并拒绝非法纳秒、负秒数和溢出 deadline，独立探针验证正常等待及 EINVAL。mutex destroy 已修复为 host 返回 EBUSY 时保留后端分配及原存储；普通/递归/errorcheck 三种 mutex 在两台 native/hybris 路径验证失败后解锁、再加锁及最终销毁，旧版本普通 mutex 返回 EBUSY 却清空存储的负对照已复现。legacy mutex lock_timeout_np 已改为 monotonic 并把超时映射为 EBUSY，bionic DSO 导入探针在两台设备验证 100ms 等待和锁复用；旧实现返回 ETIMEDOUT 的差异已复现。该入口无 LP64 native 导出，未验证 32 位 ABI 或墙钟跳变。新增 API-28 mutex_timedlock_monotonic_np hook，native/hybris 两台设备对照验证 monotonic 绝对超时、过期 deadline 和空 deadline 获取空闲锁；PI/shared 和空 deadline 跨线程等待尚未验证。新增 API-28 rwlock monotonic 读/写等待 hook；两台 native/hybris 由持锁线程与等待线程对照验证约 100ms 超时及释放后过期/空 deadline 获取。正常共享同步、其他 cond 时钟/销毁语义、rwlock 公平性和其余超时边界仍未验证。已补共享 GLES2 context 的 buffer 大小、纹理精确像素、binding 隔离及创建者 context 销毁后继续访问，三轮两台 native/hybris 均通过。已补两个不共享 context 同时在各自线程 current 后执行独立 clear/readback、join 后迁回主线程复查。独立 context 已补蓝/黄片元程序的并发主机线程 draw 与精确像素，迁移后不重绑程序/顶点属性。没有 generation 对象表或 GPU 执行重叠证明 |
| G04 / P0 | 标准 loader/layer/tool 接入 | 一个已知非法小测试被 validation 捕获；一个合法小测试零新增错误；完成一帧 capture/replay 且像素匹配，再扩大到应用。**部分落地**：可选 ICD 直接接 vendor HAL，标准 glibc loader 的 8 项 headless 用例在两台设备通过；不改写 dispatch header、不删创建链。已用标准 glibc VVL 1.4.309.0 验证合法 instance/device/buffer 生命周期零 ERROR，以及零长度 buffer 精确 VUID 并阻止进入 vendor；两台设备通过。固定 widget 的上传/绘制/读回/销毁已启用 SyncVal，两种 binding 均预期像素且零 ERROR。固定 GFXReconstruct 已完成离屏 widget 捕获/回放：正确/错误 binding 各 16×16 RGBA8，未捕获、捕获、回放三份完整图像逐字节匹配。离屏 capture 无 present frame，不能据此关闭一帧 WSI/应用捕获门槛。标准 ICD 已在现有 `window_owner` 上接入 Wayland surface，并用 `VK_ANDROID_native_buffer` 导入窗口缓冲实现本地 `VK_KHR_swapchain`（有限超时 acquire、release fence、queue）。仍拒绝 HAL 自带的 driver-owned WSI。窗口 validation/capture 已作为 ICD 路径的可选 runner 开关接入标准 VVL 与 GFXReconstruct。一加 8T `20260907T225316-dcafbe9f` / Mali `20260907T225316-bf0c6ea9` 在 create/render/resize/退役/销毁及边界探针下 `WSI_VALIDATION errors=0`；随后的窗口捕获经 `gfxrecon-replay --swapchain virtual` 回放 24 次 copy，六张 live readback 在 `20260907T230116-0e424415` / `20260907T230116-2c517ce0` 逐字节匹配。这不是第二次 present，也不需要 image alias。替代前端已补单窗口 Wayland 显示验证（见文末），尚未接成窗口捕获/回放。分组 shader 的成员名称触发旧 VVL 依赖的装饰展开死循环；新增固定源码验证层构建，仅补迭代器推进。两台设备分组绘制通过，非法 buffer 仍捕获精确 VUID；来源/补丁/实际 layer 哈希关联到运行记录，详见 [分组 SPIR-V 与验证层](tests/baseline/spirv-groups.md)。已支持定向 capture 运行并自动补齐 ICD 版本/widget 依赖，记录实际调度与 16 个捕获阶段的退出结果；两台设备普通/动态 binding 的完整图像与 shader/resource 审计再次通过。已验证标准 ICD 所需 native-buffer 基础：两台 HAL 的真实 gralloc 导入、Vulkan/CPU 双读回及 release/acquire FD 往返通过，新增 AHB 借用接口；已把 Wayland native window 的创建/清理拆成不依赖 Vulkan loader 的内部 C 接口，前端复用后两台设备的 surface 生命周期、三尺寸呈现和截图回归通过；标准 ICD 已接入实验性 FIFO swapchain：修复原始创建崩溃、native buffer 基类指针转换、旧图像引用、有限超时读事件和多交换链 semaphore 重复等待。一加 8T / Mali 的实际呈现、读回与截图及退役/超时/分配边界探针通过；窗口 VVL 与虚拟 swapchain 捕获/回放分别通过；销毁阶段错误漏报已复现并修正，原始捕获及回放图像已保留。固定工具不支持 validation/capture 同开，也不能回放边界探针注入的分配失败，runner 已提前拒绝这两种组合；详见 [validation/capture 审查](tests/wsi/validation-capture-review.md) 与 [swapchain 证据](tests/wsi/swapchain-review.md)。image alias、完整 device-group/受保护模式、任意应用捕获仍未完成；native-buffer 证据见 [native-buffer 证据](tests/baseline/native-buffer.md)。此前手写 validation chain 的结果仍已撤回 |
| G05 / P0 | 能力宣告与模拟实现脱节 | features/features2、properties/limits、extensions、format/image-format query 与 CreateDevice enable 路径一致；保留原始/有效能力差异及原因；不通过删整个 pNext 重试。**部分落地**：baseline `caps` 核对未广告 feature / 未知扩展的精确拒绝错误，并检查未启用扩展的 GDPA 返回 NULL；已保存同设备 native/hybris/ICD 的 325 个具名查询值与差集：174 个 core feature/limit/sparse 值、设备标识、设备扩展及十种格式查询。两台设备前端无差异，直接 HAL ICD 有六项 buffer/present 扩展差异；不能按全栈等价解释。已补五个 core 1.1 features2 结构、55 个 core 字段一致性、同链设备启用及 false float64 的精确拒绝；三条路径两台设备通过。已补六个 core 1.1 properties2 结构的整链/逐结构对照及旧 properties 具名字段比较；两台设备三路径保存的 198 个 features2/properties2 值一致。已将 scaled 格式策略拆为独立模块，新增默认关闭、有界的逐格式原始/整数 fetch/有效 flags 与原因记录；Adreno missing、Mali force/native 保留三种决定经独立 native 查询及实际绘制对照，详见 [格式策略证据](tests/baseline/scaled-format-policy.md)。其余扩展查询链、全部格式创建验证和其他兼容变换原因记录仍未完成 |
| G06 / P0 | 缺少 draw→资源→image 诊断链 | 用 Mali-shaped UBO 测试导出绑定/布局/内容/attachment 证据；人工注入错误 binding 后能定位首个错误 draw。**部分落地**：固定 widget 已覆盖 UBO、staging、template、普通/动态及多 descriptor 捕获和 API-input shader 关联，详见下方 G06 证据记录。任意应用首个错误 draw、runtime generation 和 WSI attachment lineage 仍未完成 |
| G07 / P1 | Vulkan 格式兼容：BC、scaled vertex、swizzle/sRGB 等 | 每个已支持格式有 golden/reference 像素；格式查询、创建、view、copy、readback、mip/layer/subregion 一致；单独覆盖 BC6H/BC7，未实现则不广告。**实验性子集**：标准 ICD 已接入 12 种 R/RG/RGBA 8/16-bit scaled 顶点格式的整数 fetch + SPIR-V 补偿；Redmi Adreno 原厂缺失格式在开启后通过固定像素探针，Mali 原生透传和强制转换对照通过。已补 float32 矩阵/固定数组的混合原生浮点与 signed/unsigned scaled 列，四种聚合形状经 Adreno/Mali 像素与验证层对照通过。已保留静态实例 divisor 链，divisor 1/2/3 经 Adreno/Mali 像素与验证层通过；Adreno 零 divisor 和非零 firstInstance 通过，Mali 相应能力为 false，明确不计覆盖；详见 [实例取数证据](tests/baseline/scaled-instancing.md)。仍非完整格式能力。BC1–BC5/BC7 已接入默认关闭的 image/view/copy 路径，按实际能力限制查询和创建；12 种格式的 mip/layer/subregion、GPU 写入、sRGB/R-B swizzle、原生 RGBA8/R16/RG16 精确采样对照及资源重置已有 Redmi/Mali 探针；BC4/BC5 另覆盖 16 位 UNORM/SNORM、线性滤波与边框默认通道。新增 BC1 RGB 边框探针后，Mali 采用原生 RGB8 修复，红米仍明确 FAIL，见 [RGB8 记录](tests/baseline/bc-rgb8.md)。BC7 内核与 Mali 图像已通过独立参考验证，Redmi BC7 sRGB 原生过滤对 CPU 参考仍失败，见 [BC7 记录](tests/baseline/bc7.md)。textureCompressionBC 仍为 false，BC6H、mutable/sparse/external/alias 等完整语义与 CTS 未验收，详见 [图像回退范围与证据](tests/baseline/bc-images.md) 和独立的 [解码核心证据](tests/baseline/bc-decode.md)。整项 G07 保持开放 |
| G08 / P1 | SPIR-V 转换缺少语义保证 | 转换前后 spirv-val、反射 diff、源码/二进制 hash、pipeline specialization key；clip/cull 真使用时正确模拟或拒绝，不能简单删除改变画面。**实验性子集**：固定 scaled 探针的原始/转换后 SPIR-V 已保留哈希、spirv-val、反汇编和接口差异；原始模块匹配构建输入，四条 loader 路径像素正确。已补多入口模块按 pipeline 指定 stage 生成临时模块，并裁剪无关函数/全局变量；同一三入口模块供顶点/片元及后续 pipeline 复用，辅助函数读取 push constant 在 Adreno/Mali 真机通过。已修复 shuffle 等明确字面量被当作变量 ID 的误判，ID=3/索引=3 的合法 shader 经旧/新 Adreno 同探针对照确认；复杂操作数布局仍保守扫描。聚合输入由独立 pass 拆为逐 Location 输入并重建原 float Private 值，动态索引/整体值传参四种形状通过；实际模块的原始哈希、逐列接口和 spirv-val 已审计。已补按 pipeline 特化参数解析直接/表达式数组长度，默认值、混合 float/bool 参数、同 module 重复创建与 cache 保存/重建在 Adreno/Mali 通过，原始特化字节与映射已关联转换模块。已补独立装饰组展开，单入口、多入口、特化数组的 Location/成员布局/SpecId 经两台原厂驱动和修复后的验证层通过，并由 SPIRV-Tools 独立核对装饰语义。通用接口/扩展、完整 specialization/cache key 仍未完成。**clip 自动路径**：物理设备 `shaderClipDistance=false` 时 ICD 广告该 feature、CreateDevice 对 HAL 剥离 enable 位，并把已使用的 ClipDistance 改写为 Location varying + 片元 discard；不需要应用传环境变量。X300 上 Blender 4.3 `is_supported` 已通过并创建窗口；画面合成/present 仍属 G11 |
| G09 / P1 | 同步/内存模型模拟不完整 | non-coherent atom 对齐与 flush/invalidate、staging 多次写入、submit 重用、queue 间信号、wait-before-signal、销毁时仍在飞行测试；没有全局 wait-idle 才能运行的默认实现。**部分验证**：Mali 同 family 双 queue 的 consumer 先提交、producer 后 signal，四轮 fill→copy→1024 words 精确读回、timeline/fence 及资源重用在 native/frontend/ICD core/KHR 六路径通过；实际非 coherent 内存整块 invalidate，标准 ICD 两种 alias 的 SyncVal 零错误。Adreno 厂商不支持所需 timeline；绕过 libhybris 的独立官方 Turnip 对照在 Redmi 只有一个 queue，此用例返回不支持。新增 Vulkan 1.0 局部上传/读回探针，Mali native/frontend/ICD 及标准 validation 四路径通过：非零绑定偏移、跨 atom 写入、部分有限范围 flush/invalidate、相邻未改动数据及四轮 command buffer 重提交，SyncVal 零错误；Adreno 原厂无适用非 coherent 内存类型，四路径不支持。新增非零偏移部分 mapping、VK_WHOLE_SIZE 到映射末尾及 allocation 尾部非整 atom 的有限/WHOLE_SIZE 范围；Mali 四路径七轮精确读回通过，SyncVal 零错误，Adreno 四路径仍因内存类型不支持。跨 family ownership、并发主机提交及其它在飞工作下的资源退役仍未覆盖；详见 [内存与同步证据](tests/baseline/memory-sync.md) |
| G10 / P1 | 关闭。产品桌面 GL 就是 Mesa/Zink，不是 hybris GL→GLES 或 Gladio | 不再作为待做项。Zink 能力差集记 G07/G08。Blender 经 Zink 不是验收条件。不重建第二套 GL 前端，不为 teapot 再接 VVL/capture |
| G11 / P1 | 产品窗口已是 ICD/Turnip WSI → TAWC-DRI / android_wlegl → anlabwc。hybris EGL X11 插件已删除，`ws_init` 拒绝 wayland/x11，不是现行旁路。**部分落地**：Xlib/XCB/Wayland 创建、FIFO release、resize/out-of-date、原生 X 销毁后 surface-lost，以及产品 teapot/scene Host 门已有证据。剩余是多窗口、minimize、断连、延迟 release、并发销毁、长期 FD，以及 advertised usage / alpha≠1 的真实操作 | 已通过的 create/render/resize/retirement/destroy 与应用像素门保持有效。剩余生命周期与竞态需要独立证据；不能靠恢复 EGL 桌面窗口插件或第二条 present 来关闭 |
| G12 / P1 | 黑屏/贴图错误没有可重复证据包 | 对指定 frame/draw 生成输入 shader、descriptor/resource、attachment 前后图、同步事件、present 记录；有容量上限，默认不开高开销捕获 |
| G13 / P2 | 多厂商/驱动版本缺少回归与 CTS 指标 | Adreno/Mali 分开存版本化 baseline；不支持/失败/crash/timeout 分栏；每项 workaround 有原始失败、修复通过、其他设备无回归。shader 审计错误已单独记录并保留探针原始退出码；旧 VVL 停顿实测记录为 TIMEOUT 142，未扩大 CTS 覆盖 |

G07 的 BC 上传模拟不等于完整 Vulkan BC 能力：要处理应用可用的 tiling/usage、copy 规则、view compatible format、内存需求和资源 alias。只支持子集就限制相应 query/creation，不能直接把 `textureCompressionBC` 整体置真。
G08 删除未使用声明与模拟实际 clip/cull 运算是两回事；输出结构/AccessChain 重写必须保持合法和语义。
G09 timeline、dynamic rendering 等可能涉及大量语义，优先透传已有功能；缺失时按目标应用拆分研究，不承诺通过几个 wrapper 提升整个 Vulkan 版本。

### 5.1 G06 证据记录

以下均为固定离屏 fixture 的证据，不能替代任意应用的诊断门槛。

- **布局与像素**：272B std140、12 vertices / 18 indices；正确 binding 像素 `255,255,0,255`，替代 binding 为 `0,255,255,0`，color→transfer→host 同步明确。动态 UBO 使用非零 descriptor base 和 dynamic offset。实际 1232B 合成 shader block 逐项检查 14 个非对称 mat4（array stride=64、column stride=16）、尾部 vec4/vec2 和 bool/int；普通/动态两组数据经 native/frontend/ICD 得到精确预期像素，ICD VVL/SyncVal 零错误。这不是 Blender 完整 instanced widget 布局。
- **上传与重用**：两种大小的 staging copy 写入未映射的 device-local UBO，复用 descriptor/资源，在 fence 完成后重录正确/替代/正确数据并逐次重提交，每种大小六次精确读回。Vulkan 1.1 core template 使用非零 payload offset 更新同一 set，前缀为有效但相反的 descriptor，同样通过更新、重录/重提交与像素检查。两条路径 ICD VVL/SyncVal 零错误。
- **捕获资源**：GFXReconstruct 导出 draw-time descriptor、完整 272B UBO、attachment 前后图，核对 update/bind 与 set/buffer/range、attachment 与 copy 源 image。两台设备普通负对照的首次像素分叉是 draw 60；动态单 binding 关联 base＋dynamic offset 与 vertex/fragment dump，首次分叉是 draw 61。未捕获、捕获和回放的完整图像一致；篡改绑定偏移或只保留 base 的离线证据被拒绝。另核对 image/view/framebuffer 创建、render pass attachment、copy 源与子资源，以及同一已提交 command buffer 的命令归属；篡改 framebuffer/view/draw 归属的离线证据被拒绝。
- **Shader 关联**：普通/动态捕获均导出 API-input SPIR-V，逐字节匹配经 probe manifest 校验的构建快照，spirv-val 通过，并保留反汇编和接口 decoration。pipeline.json 关联创建/绑定的 pipeline、layout、set allocation、render pass、shader module、入口点、二进制哈希及路径；替换 layout/module 的离线证据被拒绝。记录的 pipelineCache=0 不是驱动内部缓存 key。

已补两个 set、四个动态 UBO（含双元素数组）的独立 shader 字段与完整图像检查；捕获重建 set/binding/array 顺序、base＋dynamic offset、buffer/memory 创建绑定和双阶段完整资源字节，并把负对照关联到 set 0 / binding 3 / element 1。详见 [多 descriptor 证据](tests/baseline/widget-multi.md)。

仍未覆盖非零 firstSet/部分重绑、template 数组/多入口/KHR 别名、多队列/并行提交、一般 descriptor/pipeline 状态历史重建、驱动转换后 shader 与缓存 key。捕获局部 ID 不是 runtime generation；尚不能定位任意应用的首个错误 draw，也没有 WSI attachment lineage。

### 5.2 实际 Blender 抓帧诊断（X300，2026-09-09）

标准 GFXReconstruct 已取得 Blender 4.3.2 的两帧应用抓帧。基于标准 JSONL 与固定 Vulkan registry 的检查器，定位到 17 组 suspend/resume 之间插入 action/synchronization 命令，以及 96 次已知提交内找不到 suspension 的 resume；保留提交、command buffer 代次、attachment 与 draw 绑定关联，并可生成标准资源转储请求。fork 已补无顶点属性 indexed draw 的索引转储，实际取得 widget 的 36B 索引和 272B UBO；后者与抓帧中的 UpdateBuffer 字节一致。详见 [渲染抓帧诊断](docs/rendering-capture-analysis.md)。

另已定位诊断工具的边界：unassisted 抓帧把非 coherent 上传的 FillMemory 放在原始 flush 之后，默认回放读到全零索引；rebind 对照及标准 page_guard 重新抓帧均读回正确索引，不能把旧回放中的零数据归咎于应用或兼容层。详见 [抓帧内存可见性](docs/capture-memory-visibility.md)。实际应用和回放仍有渲染错误，临时 rendering flag/barrier 对照不算产品兼容修复；任意应用首错 draw、完整资源历史及 WSI lineage 仍未关闭。

## 6. 黑屏、贴图错误与 device lost 的诊断设计

### 6.1 三个观察边界

```text
A：应用 API 输入与声明能力
       ↓ compatibility transform
B：实际 backend 调用、shader、资源和同步
       ↓ GPU 执行
C：attachment → 最终图像 → WSI buffer → compositor 接收/显示
```

每个对象用稳定 ID + generation，每次 command-buffer 录制用独立 generation；关联 thread/context、submit、draw、frame、shader hash。
descriptor 需在实际使用时重建有效状态：普通 set、copy/update template、dynamic offsets、push descriptors，未来的 update-after-bind/descriptor buffer 依支持范围加入。只打印 pointer 或 UpdateDescriptorSets 参数不够。
对 mapped memory 不能假设所有 CPU 写入都有 API 调用；调试快照应在有合法同步的提交/使用边界捕获，记录 flush/dirty range。host coherent 也不意味着可以与 GPU 无同步读写。

按需证据包建议内容：

- `manifest.json`：source/build-id、GPU/driver、native/effective capabilities、quirk ID、输入 fixture/seed。
- `events.jsonl`：有上限的最近 N 次 draw/submit 状态环；错误、指定 frame 或显式请求触发落盘。
- `shaders/`：原始与转换后源码/SPIR-V、编译日志、反射、specialization、pipeline cache key。
- `resources/`：指定 buffer range、texture mip/layer/aspect、format/stride/swizzle/colorspace；大资源只捕获指定范围。
- `images/`：选中 pass 的前后结果、最终提交图；整型可精确比较，浮点/滤波设明确容差，sRGB/alpha 单独比较。
- `present.jsonl`：buffer ID、acquire/render-complete/release fence 状态、尺寸/generation、surface/frame ID；不把“收到 fd 的 ACK”等同 GPU 已不再读取。
- validation 信息与 device-lost 记录；支持时查询 device fault，不支持时仍保留 CPU 侧最近提交和资源图。

故障定位顺序：无 draw → 检查应用能力选择/入口分发；draw 输出错 → 检查 B 与 attachment；attachment 正确而最终 image 错 → 查 copy/blit/合成；最终 image 正确而窗口错 → 查 WSI/尺寸/颜色/fence。黑色本身可能是合法内容，必须有测试预期，不能靠颜色启发式判错。
调试插入的 readback/barrier/wait 会改变时序，证据标明启用项；保留关闭 instrumentation 的对照。默认模式不每 draw 写日志、不强制全局同步。

### 6.2 现有工具如何接入

| 工具 | 用处 | 本项目仍需验证 |
|---|---|---|
| [Vulkan Validation Layers](https://github.com/KhronosGroup/Vulkan-ValidationLayers) | API/object/参数检查；[SyncVal](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/syncval_usage.md) 检查同步 hazards | layer 链、验证边界与依赖版本；无报错不证明像素正确 |
| [GPU-assisted validation](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/gpu_validation.md) | shader 侧访问/descriptor 等错误线索 | feature 与资源预算可能不满足；会扰动 shader/执行，不能保证老设备能启用 |
| [GFXReconstruct](https://github.com/LunarG/gfxreconstruct) | Vulkan capture/replay、抽取 SPIR-V、转换 API 事件 | 先测无 WSI，再测 Android buffer/自定义 WSI；跨平台/跨驱动 replay 非普遍保证，不承诺自动生成可用最小 C 用例 |
| [RenderDoc](https://github.com/baldurk/renderdoc) | 单帧资源、pipeline、纹理与绘制检查 | glibc ARM64 + bionic driver 混合进程不是已验证目标；Android 支持不等于本链路可直接附加；legacy GL 不在其完整支持范围 |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | validate/disassemble/optimize/reduce | target-env 与转换阶段固定；合法 SPIR-V 不代表驱动执行正确或转换语义等价 |
| [VK_EXT_device_fault](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html) | 驱动支持时取 lost 后故障信息 | 29854870 清单未暴露；不作必选依赖；不能承诺恢复 lost device |

若 RenderDoc/完整 replay 接入耗时，先交付 repo 内按 draw 的 bounded state/image dump；不要为摸底先实现新的通用 capture 文件格式和完整 replay 引擎。

## 7. 独立测试与衡量方法

官方 [VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) 覆盖 Vulkan、OpenGL、GLES、EGL；作为固定版本外部依赖，按功能组逐步跑。常规摸底保持轻量，不默认下载/执行全量套件。CTS 结果与正式 Khronos conformance submission 是两回事。
用 [Vulkan Profiles](https://github.com/KhronosGroup/Vulkan-Profiles) / Zink requirements 输出声明能力差集，另用实际用例验证语义。

| 套件 | 建议内容 | 依赖 |
|---|---|---|
| smoke | 现有 baseline + 正确加载路径/dispatch 检查 | adb、glibc runtime、hybris；无窗口 |
| semantic | texture/mips/sRGB/swizzle、UBO/descriptor、多 pass、同步、shader 转换 | 同上；无窗口；每项明确 reference |
| blender-shaped | Mali widget 数据与 draw、Turnip View3D 多资源复用组合 | 普通 Vulkan/GLES 程序；不能把未复现的 probe 标成原 bug 回归通过 |
| wsi | buffer 交接、fence/release、resize、颜色、窗口生命周期 | 独立小型 bionic receiver/compositor；X11 需要匹配 Xwayland；不需要完整 ardesk 应用 |
| cts | 指定 API/version/feature 用例列表、分批运行 | 固定 CTS 和 runner；glibc 与 bionic 两种构建 |
| apps | Blender fixture、默认场景/UI、orbit/resize 与其它目标应用 | 最终集成环境；单独记录 app/version/backend |

差分至少三条：原生厂商驱动、hybris passthrough、hybris compat。前三者在同设备、同可比较 workload 下运行；模拟新增能力无法在原生执行时，对照 CPU/reference 或另一已验证实现，不把原生 NotSupported 当失败。
另外保留 Mesa/软件实现的参考结果，但不假定它能重放任何捕获。性能记录 CPU 提交时间、GPU 时间（设备支持时）、frame latency、内存峰值；改对画面与开销分开评价。

统计必须包含 tested/pass/fail/unsupported/crash/timeout，固定用例和设备版本；不同 API/profile 不汇成误导性的单一“兼容率”。每个 app bug 记录原失败图、目标 reference、首次错误 pass，而不只是能否启动。

现有 baseline 已补（2026-09-06）：

- `tools/build-aarch64.sh` 从本仓库交叉编译并写出 `manifest.json`；`build.sh` 只编 probe。
- 每次构建使用干净源码/headers 快照及 install/runtime staging；记录输入内容哈希、工具链身份和部署 ELF 集合。runner 校验库及探针 manifest，记录实际 maps 快照；Android 路径的事后哈希与内存页身份分开描述。
- exec 前记录 probe PID，超时后检查 executable 路径再清理；`alarm(25)` 在 probe constructor，不覆盖更早的依赖 constructor，因此仍需要 host timeout。
- 设备目录改为 `/data/local/tmp/libhybris-baseline-<run-id>`。

仍缺：

- 已区分 EGL_CLIENT_APIS 不支持桌面 GL 与 EGL 调用失败；仍需要更完整的 context/profile 特性测试。
- 共享 rwlock 销毁和 mutex 超时加锁已在调用 glibc 前转换 tagged handle；当前设备缺少 `/dev/shm`，成功分配后的共享路径仍未实测，不能据此关闭共享同步缺口。
- stdio 的 fgetpos/fsetpos 按 bionic 偏移量语义使用 ftello/fseeko，避免失败后读取未初始化的 glibc fpos；探针覆盖普通/64 位字节流位置恢复和管道 ESPIPE，32 位溢出、多字节状态仍未覆盖。
- stdio 的 fflush 不再以 fileno 预检查过滤 NULL/内存流；独立原生对照覆盖全局文件刷新和 open_memstream 内容发布，unlocked 入口同类修正尚未独立实测。
- 已补 rwlock kind 的 bionic/glibc 枚举转换和原生对照；仍未验证高争用下的公平性/饥饿行为。
- 已补未使用的私有静态 mutex/condition/rwlock 销毁及显式重新初始化的原生/hybris 对照；仍未覆盖共享同步对象、在用对象销毁或重复销毁。
- ES3 专用功能、Vulkan texture/compute、GLES 共享资源并发访问与共享对象在仍被引用时删除尚未测；widget shader 和 device/fence 双线程不足以代表 API feature level。
- 标准 validation/capture 已接到标准 loader 的 headless widget 与窗口探针（G04 Partial）；任意应用覆盖仍缺。已移除绕开标准 loader 的手写 layer chain，原 native 成功记录不再作为完成证据。
- G03/G05/G06 仍只有 headless probe；可选 ICD 有 instance/device generation 表，尚无 resource generation 或完整应用证据包。`unload` 现在能正常退出，是因为 hooks DSO 被钉住，不是因为 Android Vulkan 对象可回收。`init`/`tls` 检查并发首次 `android_dlopen` 得到完整结果，以及 frontend 关闭后工作线程能退出；另由 `tls-dtor` 观测两种 TLS 模型的实际 C++ 析构，`tls-bounds` 检查跨线程注册的初值补齐与本地修改保留。它们不证明映射消失、静态槽回收或所有 vendor TLS 析构正确。

## 8. 建议实施顺序与完成门槛

1. **建立可信基线（G01–G05）**：独立构建/运行、产物 manifest、统一 dispatch、loader/layer spike。门槛：小型合法 workload 完整通过；故意错误被诊断；原生与 passthrough 差异可解释。
2. **先补诊断（G06/G12）**：Mali widget UBO/readback 与 draw state、attachment lineage、shader dump。门槛：注入错误绑定/颜色转换后能指出首个失败阶段，不再只得到一张白屏截图。
3. **按证据增加兼容功能（G07–G09）**：优先 BC 常见子集、scaled vertex、已知 shader/driver quirk；每项 capability 精确门控、单独开关和 reference。移植前先跑参考 Vortek 算法对应小测试。
4. **WSI（G11）**：G10 已关闭，产品桌面 GL 就是 Zink。窗口提交已统一到标准 loader → ICD/Turnip → TAWC-DRI / android_wlegl。G11 剩余是多窗口与生命周期竞态，不是第二条 present。desktop-gl 离屏不能代替应用窗口门。
5. **扩大真实应用/设备回归（G13）**：在原故障设备重现并闭环两个 Blender 案例；Turnip 原 bug 是否修复与 hybris 兼容性分开结论。一个 Adreno 650 的通过不能替代 Mali/Adreno 830 的结果。

每个 workaround 必须包含：触发条件（vendor/device/driver/feature）、为何需要、转换语义、开启/关闭结果、回归用例、额外成本和撤销条件。原始 shader、capability、pNext 不可悄悄丢弃。
当前不宜承诺的结果：完整 OpenGL 4.x/Vulkan 1.3+、任意 app 都正确、所有 GPU 黑屏自动诊断根因、纯 libhybris 修复 Turnip 内核/编译器缺陷。先建立能定位与验证的兼容平台，再逐项扩大支持面。

逐日实验记录与 run ID 见 [实施状态](IMPLEMENTATION.md) 和各 `tests/` 文档，不在本清单后续写。
