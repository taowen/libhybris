# libhybris 图形兼容性与渲染诊断缺口

调研日期：2026-09-06。当前 libhybris 基准：`dcc3588262a2b5181a15998c63d5e7e684ef2631`。
范围：评估把本仓库扩展为同进程的 GLES / 桌面 OpenGL / Vulkan 兼容栈；借鉴 Vortek、Gladio 的能力，不照搬它们的命令 IPC。
本文最初为调研建议，现作为持续实施的验收清单；阶段进度见 [实施状态](IMPLEMENTATION.md)。下文目标结构和完整兼容层尚未完成，两次 Blender 故障尚未重新复现。

## 1. 结论与边界

当前 libhybris 已能把 glibc 程序接到 Android GPU 驱动，但它还不是 Vortek 式 Vulkan 语义兼容层，也不是 Gladio 式桌面 GL 实现。基础测试通过与复杂应用正确渲染之间，主要缺少：

1. 完整、可验证、不会绕过包装的 API 分发与对象管理。
2. 可运行标准 validation/capture 工具的加载链。
3. 从一次 draw/submit 追到实际 descriptor、内存内容、attachment 和最终呈现缓冲区的证据链。
4. 有真实能力约束、有像素回归测试的格式/着色器/同步兼容处理。
5. 桌面 GL 前端，以及独立于 GPU 后端的窗口提交机制。

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
记录在 [基线说明](tests/baseline/README.md)；原始 JSON/log 留在本地被忽略的 `tests/baseline/build/results/`。本轮未重跑完整应用。

### 2.2 当前代码，而非历史实验或旧产物

| 模块 | 当前证据 | 不能据此推断 |
|---|---|---|
| [Vulkan 包装](hybris/vulkan/vulkan.c) / [导出](hybris/vulkan/vulkan_exports.c) | 独立文件持有导出 trampoline，GIPA/GDPA 对少数 WSI 函数拦截，其余转发 | 所有新加兼容处理都会覆盖静态链接、dlsym、GIPA、GDPA 四种入口 |
| [Vulkan 平台构建](hybris/vulkan/platforms/Makefile.am) | common/null/wayland；Xlib/XCB surface 在 Vulkan 包装中返回不支持 | 历史 `vulkanplatform_x11.so` 仍是当前源码能力 |
| [GLES 包装](hybris/glesv2/glesv2.c) | GLES 导出与 Android 库桥接 | 提供桌面 OpenGL core/compat、GLX 或完整 GL→GLES 转换 |
| [EGL X11](hybris/egl/platforms/x11/x11_window.cpp) | `TAWC-DRI` 与 `m_present_sock` 分支并存；后者发 AHB3 私有消息 | EGL X11 已只有统一窗口协议，或已经支持 Vulkan X11 |
| [Vulkan Wayland](hybris/vulkan/platforms/wayland/wayland_window.cpp) | android_wlegl 缓冲区提交和 fence 等待 | 已通过 resize、surface lost、多窗口和 compositor release 压力测试 |
| [loader bridge](hybris/common/linker_bridge.c)、[libc hooks](hybris/common/hooks.c)、[同步桥接](hybris/common/bionic_sync.c)、[TLS 说明](TAWC_FORK.md) | 独立 Android linker、libc/线程桥接、ARM64 TLS thunk | 任意 Android 版本、任意 vendor library 都兼容 |

摸底中发现本机旧安装目录残留 `vulkanplatform_x11.so`，但当前源码没有相应构建目标。以后必须干净 staging，并记录实际加载文件的 build-id/SHA256，不能拿安装目录文件名证明源码功能。

### 2.3 Vortek / Gladio 能借鉴什么

本地 Vortek client `f2c50d8`、Gladio client `58d21ab`；宿主实现位于父项目。这些是设计参考，不是已经通过 CTS 的正确性 oracle。

| 参考 | 观察到的能力 | 移植时的缺口 |
|---|---|---|
| [Vortek ShaderInspector](../../android/app/src/main/cpp/vortekrenderer/src/shader_inspector.c) | scaled vertex format 转换、SPIR-V 处理、按 Mali/DXVK 条件启用处理 | 要去除 RPC object 依赖；转换前后验证、语义等价、设备/驱动条件与关闭开关 |
| [Vortek TextureDecoder](../../android/app/src/main/cpp/vortekrenderer/src/texture_decoder.c) | BC 解码与替代 image/upload；当前 `getBCInfo` 明列 BC1–BC5 | 不是完整 BC1–BC7 支持证明；还需 mip/layer/subregion/sRGB/SNORM 等语义 |
| [timeline 模拟](../../android/app/src/main/cpp/vortekrenderer/src/timeline_semaphore.c) | 软件状态和 submit 过滤逻辑 | 多 queue、wait-before-signal、GPU 完成可见性与生命周期必须独立验证，不能原样当规范实现 |
| [Gladio 自述](../gladio/README.md) | GL 1.x 模拟、shader 转换、纹理解压，重点为旧游戏 | 是 client 项目自述，不代表父项目移植的宿主具备全部功能 |
| [当前 Gladio 宿主](../../android/app/src/main/cpp/gladio_host.c) | GLES context、部分命令处理；switch 默认分支直接跳过 | 必须逐 API 清点，不能靠现有宿主推定完整桌面 GL 兼容 |

Vortek 的封包不能改善 API 语义；其可复用价值在 compatibility algorithms。把算法改成同进程调用 backend dispatch，不需要保留 command ring/socket。
若移植代码，逐文件保留许可证与来源；两个 client 根目录许可证为 LGPL-2.1，宿主源码另行按文件核对。这是移植边界记录，不影响本次只读调研。

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
桌面 GL 应用 ─→ Mesa/Zink 或独立 GL→GLES 前端 ─┐
GLES 应用 ────→ GLES compatibility + diagnostics ├→ 同进程 ABI bridge → Android 驱动
Vulkan 应用 ──→ Vulkan compatibility + diagnostics┘
                          │
                          └→ 独立 WSI / buffer lifecycle
                               ├→ Wayland 扩展 → compositor
                               └→ X11 buffer protocol → Xwayland → compositor
```

API 兼容处理不认识窗口 XID/合成器私有 socket；WSI 不修改 shader 或伪装 GPU feature。AHB 是可共享缓冲区，不是命令协议。
Wayland 的 `android_wlegl` 和 X11 的 `TAWC-DRI` 都是双方必须实现的扩展，不是核心协议自动支持 AHB。标准 DRI3/dma-buf 适配需要另行证明格式、modifier、同步与句柄兼容，不能把 AHB fd 直接等同 dma-buf。

### 4.1 Vulkan loader / ICD 决策必须先验证

长期优先评估：保留标准 glibc Vulkan loader，新增 hybris ICD adapter，compatibility 作为可组合模块/layer，使标准 validation 和 capture 有正常入口。ICD 需实现协商、dispatchable object、physical-device proc 查询和 surface/WSI 约定，不能只给现在的 `libvulkan.so` 写一份 JSON。[Khronos loader-driver 接口](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)

已验证 headless 初步路径：标准 glibc loader → 可选 hybris ICD → vendor Vulkan HAL，在两台 Adreno 650 设备通过 8 项基础探针。标准 validation 及离屏 widget capture/replay 已验证；WSI 尚未完成。Android loader 已在 bridge 下游，必须验证双 loader 的 handle/dispatch 所有权，防止递归加载同名 libvulkan；Android 库用独立路径/命名空间明确解析。
短期可先整理当前 frontend 的统一 dispatch，再做 adapter spike。若继续以替代 `libvulkan.so` 方式交付，也必须实测 layer chaining；`VK_LAYER_PATH` 不是自动接入的保证。

compat 在标准 layer 里时，验证原始 app 调用和转换后 backend 调用需要分别安排验证边界；位于 ICD 内的内部转换不会自然被上游 layer 看到。glibc layer 不能直接丢给 bionic loader 加载。

### 4.2 OpenGL 路线选择

| 路线 | 价值 | 明确限制 |
|---|---|---|
| Mesa/Zink → hybris Vulkan | 优先评估现代桌面 GL，复用完整 GL 状态机/编译栈 | 取决于所固定 Mesa 版本的 Vulkan feature/format/limit 要求，不能仅看 Vulkan 版本号 |
| GL→GLES 独立前端，借鉴 Gladio | 旧 GL/兼容 profile、厂商 Vulkan 能力不足的设备 | 固定管线、GLSL、texture/FBO、GLX/context 语义工作量大，不承诺小改动支持现代 Blender |
| GL4ES | 可比较的旧 GL 参考实现 | 目标 GL 1.5/2.1，不是现代桌面 GL 的完整替代 |
| ANGLE | GLES 实现与 shader/driver workaround 的参考/可选层 | 不是桌面 GL 实现，也不能凭空补全底层 Vulkan 功能 |

固定 Mesa commit 后，用 Zink requirements/profile 检查器得出按 GL 目标版本的差集；最新文档已包含多个扩展要求，不采用“Vulkan 1.3+ 必然够用”的经验判断。[Zink](https://docs.mesa3d.org/drivers/zink.html)、[GL4ES](https://github.com/ptitSeb/gl4es)、[ANGLE](https://github.com/google/angle)

29854870 的 GLES 基础路径可作为 GL→GLES 实验底座；它缺少多项现代 Vulkan 扩展，Zink 能力要实际测量。尚未建立该设备的桌面 GL 兼容等级。

## 5. Gap 清单与验收条件

P0 = 兼容增强前的基础；P1 = 直接影响目标应用；P2 = 基础可用后的扩大覆盖。以下是待做项，不是已实现功能。

| ID / 优先级 | 缺口与风险 | 最小验收证据 |
|---|---|---|
| G01 / P0 | 自主构建、固定输入与实际产物来源 | 独立 checkout 构建脚本，固定 headers/compiler/deps；运行 manifest 含实际 ELF hash/build-id、driver、设备、env、quirk 配置；干净安装无旧平台库。**AArch64 baseline 已验证**：本仓库固定 base digest / Debian snapshot 和 Android headers commit；构建前源码/headers 快照哈希、容器 ID、包版本、编译器及配置均留档。探针有独立 manifest，运行记录命令、driver 字符串与分阶段 mappings，关联部署哈希并对 Android 路径事后取哈希。独立 checkout 在两台设备均 21 PASS / 2 UNSUPPORTED；详见 baseline README。映射快照不等于全生命周期追踪或内存页校验 |
| G02 / P0 | 全入口 dispatch、core/KHR alias、每 instance/device 的真实函数表；避免包装绕过/递归/NULL branch | 从 vk.xml 固定版本生成覆盖表；同一测试经 link/dlsym/GIPA/GDPA；BeginRendering/KHR、Submit2/KHR 等按启用能力测试；未支持符号符合规范，不假成功。**部分落地**：普通 GIPA/GDPA 查询保留 backend 对 instance/device 的解析结果，只替换必要前端/WSI 包装；新增作用域/未启用扩展的负例测试，未解析的直接导出调用有错误信息。已从固定 Vulkan-Headers v1.4.309 registry 生成 726 个命令的 scope/alias/provider 清单；真机记录 dlsym/GIPA(NULL)/GIPA(instance)/GDPA，检查 137 个 core 1.0 必需入口及非 global/non-device 的禁止作用域。相同 fill/fence/readback 已经 link/dlsym/GIPA/GDPA 实际执行，memory2 的 core 1.1/KHR 已启用并验证。swapchain 包装已按当前 device 的 GDPA 解析并在缺失时先拒绝，Wayland 创建/销毁按当前 instance 查询（仅构建验证）；未启用扩展的直接导出负例不再返回成功。可选标准 ICD 已保存 instance/device resolver/destructor 与各自唯一 generation，物理设备普通枚举及 core/KHR group 枚举登记所属 instance，并由创建/查询/销毁入口使用；替代前端已登记 device/queue/pool/command buffer 归属，并按实际 device 的 GDPA 分发四组 core/KHR 渲染命令；X300 的固定绘制在四条前端入口均通过（详见文末）。timeline 三组 core/KHR 主机命令也已按传入 device 解析并实际执行。前端完整 resource/generation 状态、全部 alias、完整渲染/同步语义仍未覆盖 |
| G03 / P0 | 多线程/多 context/多 device 与对象生命周期 | 并行 create/destroy、二次 init/dlopen、回调、线程 TLS、handle 重用有回归；对象 state 按 generation 识别，不能用进程全局单一 current device。**部分落地**：谁持有谁：glibc 持有 `libhybris-common`（`DF_1_NODELETE`）至进程结束；common 持有 Android linker plugin，不 `dlclose` 它；frontend 持有自身 glibc 引用；vendor 对象由 `android_dlopen` 调用方持有，frontend 关闭不回收它们。允许的关闭顺序：销毁 Vulkan device/instance，丢掉 frontend 引用，进程退出。hook 表首次 qsort 已改为 pthread_once 后发布，避免无锁 sorted 标志导致排序/搜索竞争；现有并发初始化回归通过，但内部 lookup 未导出，未独立动态复现首次排序竞争。callback 发布与查询快照、缺失符号编号已改为原子操作，未命中日志开关改为 pthread_once；公开接口注明 callback 替换不等待旧调用结束，生命周期仍由调用方保证。这些共享状态修复仅有代码审查和现有回归证据，未独立复现 callback 替换竞争。`ENSURE_LINKER_IS_LOADED()` 改为 `pthread_once`：审查现有 bundled linker 初始化路径未发现重入公开 `android_*` wrapper，后续新增回调必须继续遵守该约束；`_hybris_hook_dlerror` 改为直接调 `_android_dlerror`，避免 once 期间重入。baseline `life` 覆盖双 device、destroy/recreate、二次 dlopen、两线程 create/destroy，但不覆盖首次进入。`init` 覆盖两线程同时第一次 `android_dlopen`；`tls` 覆盖工作线程完成 Vulkan 创建/销毁后主线程关闭 frontend、再让工作线程退出；已用 bionic C++ DSO 的 emulated TLS 和原生 TLSDESC 两种产物观测初值与析构；原生变体曾在 glibc 工作线程首次读到 0，现由 Q linker 静态 TLSDESC resolver 先初始化线程并重放 .tdata 修复。关闭主引用后，析构在所属线程恰好执行一次、值正确，三轮均通过；不证明 compat allocation 清理、IE TLS/signal 场景或静态槽回收。`20260906T233143-5f5b13ed`：hybris `init`/`tls`/`unload` PASS；native `init` 无对应实现，不再调度；hybris common 加载失败报 FAIL。注入第二次 pthread_create 失败后能释放 barrier、join 已启动线程并退出 2。`dlclose` 返回 0 不等于已解除映射；进程退出 0 不等于资源已回收；重复创建成功不等于 generation 管理。可选 ICD 新增 instance generation 表，四线程 16 次创建/销毁记录在两台设备配对且无遗留；探针进一步要求每轮四个 instance 同时存活；另有应用 allocator 三轮创建/查询/销毁无未释放分配、拒绝分配返回 OUT_OF_HOST_MEMORY 的三路径对照；标准路径失败可能在 loader 层发生；另有直接 ICD 探针验证状态记录首笔分配失败仅调用一次 allocator、恢复后三轮生命周期无遗留，仍不代表每个 HAL 分配点或 allocator 内部锁交互；分配回调按规范不得调用 Vulkan 命令。上述旧证据仅覆盖 instance；新 device 表在 life 的 19 次生命周期中核对 generation、父 instance、销毁配对及零遗留，观测到原始 device handle 复用。直接 ICD allocator 探针覆盖普通/core/KHR group 三种枚举后的 device 状态首笔分配拒绝及恢复；resource 状态、物理设备记录分配失败和多 GPU 仍未覆盖。已补 GLES 两个不共享 context 的 buffer/clear state 隔离、pbuffer 精确像素、主线程→工作线程→主线程迁移和三轮销毁重建，两台设备 native/hybris 均通过；补充 `vk-init` 四线程同时首次枚举/创建并各自完成四轮 instance 查询/销毁，平台初始化与全局入口解析改为 pthread_once；null 平台真机验证，Wayland 仅构建验证。静态 Android mutex 改为受锁保护的唯一后端指针发布，兼容四字节对齐；bionic DSO 的 32 把新锁、四线程首次竞争在两台设备通过，旧 common 负对照超时。已补 rwlock 唯一后端发布，32 把新锁四线程写互斥及同时读持锁/try-write 拒绝；cond 的首次指针发布也已串行化，并补 32 个新条件变量的 timedwait/signal/broadcast 正常唤醒探针；旧版本次亦通过，未复现丢失唤醒。缺少 glibc /dev/shm 时 allocator/translation 不再空指针崩溃，三种共享同步对象初始化返回 ENOMEM；这不代表正常共享同步已支持。两个 monotonic cond 别名已修复为显式 CLOCK_MONOTONIC，100ms deadline 不再立即超时；relative 入口改为 monotonic 并拒绝非法纳秒、负秒数和溢出 deadline，独立探针验证正常等待及 EINVAL。mutex destroy 已修复为 host 返回 EBUSY 时保留后端分配及原存储；普通/递归/errorcheck 三种 mutex 在两台 native/hybris 路径验证失败后解锁、再加锁及最终销毁，旧版本普通 mutex 返回 EBUSY 却清空存储的负对照已复现。legacy mutex lock_timeout_np 已改为 monotonic 并把超时映射为 EBUSY，bionic DSO 导入探针在两台设备验证 100ms 等待和锁复用；旧实现返回 ETIMEDOUT 的差异已复现。该入口无 LP64 native 导出，未验证 32 位 ABI 或墙钟跳变。新增 API-28 mutex_timedlock_monotonic_np hook，native/hybris 两台设备对照验证 monotonic 绝对超时、过期 deadline 和空 deadline 获取空闲锁；PI/shared 和空 deadline 跨线程等待尚未验证。新增 API-28 rwlock monotonic 读/写等待 hook；两台 native/hybris 由持锁线程与等待线程对照验证约 100ms 超时及释放后过期/空 deadline 获取。正常共享同步、其他 cond 时钟/销毁语义、rwlock 公平性和其余超时边界仍未验证。已补共享 GLES2 context 的 buffer 大小、纹理精确像素、binding 隔离及创建者 context 销毁后继续访问，三轮两台 native/hybris 均通过。已补两个不共享 context 同时在各自线程 current 后执行独立 clear/readback、join 后迁回主线程复查。独立 context 已补蓝/黄片元程序的并发主机线程 draw 与精确像素，迁移后不重绑程序/顶点属性。没有 generation 对象表或 GPU 执行重叠证明 |
| G04 / P0 | 标准 loader/layer/tool 接入 | 一个已知非法小测试被 validation 捕获；一个合法小测试零新增错误；完成一帧 capture/replay 且像素匹配，再扩大到应用。**部分落地**：可选 ICD 直接接 vendor HAL，标准 glibc loader 的 8 项 headless 用例在两台设备通过；不改写 dispatch header、不删创建链。已用标准 glibc VVL 1.4.309.0 验证合法 instance/device/buffer 生命周期零 ERROR，以及零长度 buffer 精确 VUID 并阻止进入 vendor；两台设备通过。固定 widget 的上传/绘制/读回/销毁已启用 SyncVal，两种 binding 均预期像素且零 ERROR。固定 GFXReconstruct 已完成离屏 widget 捕获/回放：正确/错误 binding 各 16×16 RGBA8，未捕获、捕获、回放三份完整图像逐字节匹配。离屏 capture 无 present frame，不能据此关闭一帧 WSI/应用捕获门槛；标准 ICD WSI 未实现。替代前端已补单窗口 Wayland 显示验证（见文末），尚未接成窗口捕获/回放。此前手写 validation chain 的结果仍已撤回 |
| G05 / P0 | 能力宣告与模拟实现脱节 | features/features2、properties/limits、extensions、format/image-format query 与 CreateDevice enable 路径一致；保留原始/有效能力差异及原因；不通过删整个 pNext 重试。**部分落地**：baseline `caps` 核对未广告 feature / 未知扩展的精确拒绝错误，并检查未启用扩展的 GDPA 返回 NULL；已保存同设备 native/hybris/ICD 的 325 个具名查询值与差集：174 个 core feature/limit/sparse 值、设备标识、设备扩展及十种格式查询。两台设备前端无差异，直接 HAL ICD 有六项 buffer/present 扩展差异；不能按全栈等价解释。已补五个 core 1.1 features2 结构、55 个 core 字段一致性、同链设备启用及 false float64 的精确拒绝；三条路径两台设备通过。已补六个 core 1.1 properties2 结构的整链/逐结构对照及旧 properties 具名字段比较；两台设备三路径保存的 198 个 features2/properties2 值一致。其余扩展查询链、全部格式创建验证和兼容变换原因记录仍未完成 |
| G06 / P0 | 缺少 draw→资源→image 诊断链 | 用 Mali-shaped UBO 测试导出绑定/布局/内容/attachment 证据；人工注入错误 binding 后能定位首个错误 draw。**部分落地**：固定 widget 已覆盖 UBO、staging、template、普通/动态捕获和 API-input shader 关联，详见下方 G06 证据记录。任意应用首个错误 draw、runtime generation 和 WSI attachment lineage 仍未完成 |
| G07 / P1 | Vulkan 格式兼容：BC、scaled vertex、swizzle/sRGB 等 | 每个已支持格式有 golden/reference 像素；格式查询、创建、view、copy、readback、mip/layer/subregion 一致；单独覆盖 BC6H/BC7，未实现则不广告 |
| G08 / P1 | SPIR-V 转换缺少语义保证 | 转换前后 spirv-val、反射 diff、源码/二进制 hash、pipeline specialization key；clip/cull 真使用时正确模拟或拒绝，不能简单删除改变画面 |
| G09 / P1 | 同步/内存模型模拟不完整 | non-coherent atom 对齐与 flush/invalidate、staging 多次写入、submit 重用、queue 间信号、wait-before-signal、销毁时仍在飞行测试；没有全局 wait-idle 才能运行的默认实现 |
| G10 / P1 | 桌面 GL frontend | 分别声明 core/compat 版本；GLSL/link、VAO/VBO、UBO/SSBO、FBO、sRGB、texture、GLX/EGL contexts 回归；经 Zink/GL→GLES 的失败归属独立统计 |
| G11 / P1 | Vulkan X11 缺失，EGL X11 含旁路，WSI release 正确性未证实 | Xlib/XCB/Wayland 分别创建、多窗口、resize/minimize、out-of-date、surface destroy/recreate；无提前复用，无 FD 泄漏，帧 ID 贯穿 compositor |
| G12 / P1 | 黑屏/贴图错误没有可重复证据包 | 对指定 frame/draw 生成输入 shader、descriptor/resource、attachment 前后图、同步事件、present 记录；有容量上限，默认不开高开销捕获 |
| G13 / P2 | 多厂商/驱动版本缺少回归与 CTS 指标 | Adreno/Mali 分开存版本化 baseline；不支持/失败/crash/timeout 分栏；每项 workaround 有原始失败、修复通过、其他设备无回归 |

G07 的 BC 上传模拟不等于完整 Vulkan BC 能力：要处理应用可用的 tiling/usage、copy 规则、view compatible format、内存需求和资源 alias。只支持子集就限制相应 query/creation，不能直接把 `textureCompressionBC` 整体置真。
G08 删除未使用声明与模拟实际 clip/cull 运算是两回事；输出结构/AccessChain 重写必须保持合法和语义。
G09 timeline、dynamic rendering 等可能涉及大量语义，优先透传已有功能；缺失时按目标应用拆分研究，不承诺通过几个 wrapper 提升整个 Vulkan 版本。

### 5.1 G06 证据记录

以下均为固定离屏 fixture 的证据，不能替代任意应用的诊断门槛。

- **布局与像素**：272B std140、12 vertices / 18 indices；正确 binding 像素 `255,255,0,255`，替代 binding 为 `0,255,255,0`，color→transfer→host 同步明确。动态 UBO 使用非零 descriptor base 和 dynamic offset。实际 1232B 合成 shader block 逐项检查 14 个非对称 mat4（array stride=64、column stride=16）、尾部 vec4/vec2 和 bool/int；普通/动态两组数据经 native/frontend/ICD 得到精确预期像素，ICD VVL/SyncVal 零错误。这不是 Blender 完整 instanced widget 布局。
- **上传与重用**：两种大小的 staging copy 写入未映射的 device-local UBO，复用 descriptor/资源，在 fence 完成后重录正确/替代/正确数据并逐次重提交，每种大小六次精确读回。Vulkan 1.1 core template 使用非零 payload offset 更新同一 set，前缀为有效但相反的 descriptor，同样通过更新、重录/重提交与像素检查。两条路径 ICD VVL/SyncVal 零错误。
- **捕获资源**：GFXReconstruct 导出 draw-time descriptor、完整 272B UBO、attachment 前后图，核对 update/bind 与 set/buffer/range、attachment 与 copy 源 image。两台设备普通负对照的首次像素分叉是 draw 60；动态单 binding 关联 base＋dynamic offset 与 vertex/fragment dump，首次分叉是 draw 61。未捕获、捕获和回放的完整图像一致；篡改绑定偏移或只保留 base 的离线证据被拒绝。另核对 image/view/framebuffer 创建、render pass attachment、copy 源与子资源，以及同一已提交 command buffer 的命令归属；篡改 framebuffer/view/draw 归属的离线证据被拒绝。
- **Shader 关联**：普通/动态捕获均导出 API-input SPIR-V，逐字节匹配经 probe manifest 校验的构建快照，spirv-val 通过，并保留反汇编和接口 decoration。pipeline.json 关联创建/绑定的 pipeline、layout、set allocation、render pass、shader module、入口点、二进制哈希及路径；替换 layout/module 的离线证据被拒绝。记录的 pipelineCache=0 不是驱动内部缓存 key。

仍未覆盖多 dynamic binding、template 数组/多入口/KHR 别名、多队列/并行提交、一般 descriptor/pipeline 状态历史重建、驱动转换后 shader 与缓存 key。捕获局部 ID 不是 runtime generation；尚不能定位任意应用的首个错误 draw，也没有 WSI attachment lineage。

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
- 标准 validation/capture 接入（G04）尚未完成；已移除绕开标准 loader 的手写 layer chain，原 native 成功记录不再作为完成证据。
- G03/G05/G06 仍只有 headless probe；可选 ICD 有 instance/device generation 表，尚无 resource generation 或完整应用证据包。`unload` 现在能正常退出，是因为 hooks DSO 被钉住，不是因为 Android Vulkan 对象可回收。`init`/`tls` 检查并发首次 `android_dlopen` 得到完整结果，以及 frontend 关闭后工作线程能退出；另由 `tls-dtor` 观测两种 TLS 模型的实际 C++ 析构，`tls-bounds` 检查跨线程注册的初值补齐与本地修改保留。它们不证明映射消失、静态槽回收或所有 vendor TLS 析构正确。

## 8. 建议实施顺序与完成门槛

1. **建立可信基线（G01–G05）**：独立构建/运行、产物 manifest、统一 dispatch、loader/layer spike。门槛：小型合法 workload 完整通过；故意错误被诊断；原生与 passthrough 差异可解释。
2. **先补诊断（G06/G12）**：Mali widget UBO/readback 与 draw state、attachment lineage、shader dump。门槛：注入错误绑定/颜色转换后能指出首个失败阶段，不再只得到一张白屏截图。
3. **按证据增加兼容功能（G07–G09）**：优先 BC 常见子集、scaled vertex、已知 shader/driver quirk；每项 capability 精确门控、单独开关和 reference。移植前先跑参考 Vortek 算法对应小测试。
4. **桌面 GL 与 WSI（G10/G11）**：先确定目标 GL profile 和 Zink 差集；补 Vulkan X11、统一窗口提交。工具和测试仍独立，最后才接 ardesk 验证。
5. **扩大真实应用/设备回归（G13）**：在原故障设备重现并闭环两个 Blender 案例；Turnip 原 bug 是否修复与 hybris 兼容性分开结论。一个 Adreno 650 的通过不能替代 Mali/Adreno 830 的结果。

每个 workaround 必须包含：触发条件（vendor/device/driver/feature）、为何需要、转换语义、开启/关闭结果、回归用例、额外成本和撤销条件。原始 shader、capability、pNext 不可悄悄丢弃。
当前不宜承诺的结果：完整 OpenGL 4.x/Vulkan 1.3+、任意 app 都正确、所有 GPU 黑屏自动诊断根因、纯 libhybris 修复 Turnip 内核/编译器缺陷。先建立能定位与验证的兼容平台，再逐项扩大支持面。


Device-state 补充边界：扩展 allocator 探针的初次运行
`20260907T051844-72e121b6` / `20260907T051844-288595ec` 中，native 与替代
libvulkan 在 KHR group 枚举后使用 physical handle 时均 SIGSEGV；直接 ICD
与标准 loader ICD 的同一路径通过。尚未定位 Android loader/driver 根因，
不能宣称前两条路径的 KHR group 创建可用。最终 allocator 回归对 native、
替代前端及标准 loader 使用普通物理设备枚举；直接 ICD 单独覆盖普通/core/KHR
三种枚举。上述崩溃是保留的未解决缺口，不计作最终回归已修复项。


### 当前推进状态（红米 / X300，2026-09-07）

后续只在红米 `29854870` 和 vivo X300 `10AFA31610002QH` 验证，不使用
一加 8T。APK 安装使用 `../../tools/install-apk.sh --serial SERIAL APK`；该脚本
识别 X300 并处理 OriginOS USB 安装弹窗。headless runner 不安装 APK。

- G02 的 KHR group 缺陷已在替代前端修复：保留 downstream KHR 可用性检查，
  优先调用 loader 的 core group 包装，避免直接 HAL 返回的 physical handle
  跳过 loader 初始化。GIPA 与 ELF 导出同用包装，不改写 handle 头。红米旧库
  GIPA SIGSEGV、ELF abort；修复后两台 API 1.0+KHR / 1.1+KHR 的设备创建通过。
  原生 Android loader 在两台仍崩溃，保留为 CRASH；没有作为 UNSUPPORTED 隐去。
- X300 frontend 初次加载缺少 `/system_ext/lib64/libgpud_sys.so`；AArch64 构建
  默认搜索路径补入 `/system_ext/lib64` 后通过。加载失败现在明确输出 dlerror。
- 红米完整运行 `20260907T060132-e4dca178`：95 PASS / 4 UNSUPPORTED / 1 CRASH
  （native-groups）；原有 validation、SyncVal、两种 capture/replay 均通过。
- X300 完整运行 `20260907T060132-4afa862a`：79 PASS / 4 UNSUPPORTED /
  16 CRASH / 1 FAIL。除 native-groups 外，还有 frontend vk-init/TLS、ICD
  vk-init、ICD widget pipeline 创建及其 validation/capture 路径崩溃，另有
  ICD core11 transfer 失败。frontend widget 通过。尚未定位这些 Mali 问题，
  不能用此前 Adreno 的通过记录证明 X300 可用。

下一验收里程碑：先打通 X300 的 ICD graphics pipeline 和并发初始化/TLS，
完成同设备 native/frontend/ICD 定向对照；随后接最小窗口及 release/resize，
再闭合一个真实应用故障。固定离屏通过仍不等于 G04/G06/G11/G12 完成。
开发可用 runner 的重复 `--case BACKEND-MODE` 参数选择用例，保留 manifest、
逐项分类与清理；默认完整运行不变，定向运行不与 capture 同用。


2026-09-07 TLS 首次访问补充：AArch64 MRS thunk 在 glibc 新线程尚未建立
bionic TLS slot 1 时调用保存寄存器/标志的初始化 helper，并填写 pthread
shadow 的 tid 前缀。独立 `tls-mrs` 探针在八个新线程验证首次进入、部分
整数/NEON 状态、NZCV、errno 和 tid；X300 旧库负对照
`20260907T062749-34d32fa1` 八线程均失败，新库两台均通过。
完整红米 `20260907T062705-eb2706e7` 为 96 PASS / 4 UNSUPPORTED / 1 CRASH；
X300 `20260907T062706-9fb06554` 为 83 PASS / 4 UNSUPPORTED / 13 CRASH / 1 FAIL。
X300 frontend vk-init/TLS 和 ICD vk-init 已通过；native-groups、ICD widget、
widget validation、capture 崩溃及 core11 transfer 失败仍未解决。
该修补不证明 signal/fork、SVE/SME、所有 Android pthread 布局或后加载模块
的 IE TLS 初值重放。下一步仍需打通 X300 ICD graphics pipeline 和 core11
传输，再继续 G02/G04 的应用与 WSI 验收；不能据此关闭 G03。


2026-09-07 调试效率补充：新增 [保存用例的一键 LLDB 取证](tools/DEBUGGING.md)，
固定保存产物、命令、工具哈希和设备 fingerprint，自动收集停机 mappings、
ELF/build-id、线程栈/寄存器与有限内存；显式加载独立 Android linker 的模块
后能恢复闭源驱动的 unwind 链。红米正常绘制退出 0、X300 管线崩溃及超时
清理已实际验证，不要求 root。现有构建已有 DWARF，无需新增构建模式。
X300 `debug-icd-ubo-d6afd0a4` 位于 `20260907T062706-9fb06554` 结果目录，
确认 Mali `0xa237bc` 读取 instance 相关对象头的 `0x1cdc0de`，故障地址
`0x1cdc1de`；支持 MMUD 路径依赖 Android loader 私有数据的判断，尚未修复。
重新实际构建后，红米 `20260907T064509-e1042b03` 的 tls-mrs/core11/ubo
三项通过；X300 `20260907T064510-08fcee5d` 仍为 tls-mrs PASS、core11 FAIL、
ubo CRASH。没有把成功取证计为兼容性通过，也未改变 G02/G04/G06 门槛。

2026-09-07 X300 MMUD 兼容选项：新增独立 `mali_quirks.c`，仅显式开启
`HYBRIS_MALI_MMUD_SKIP_LOADER_CHECK=1` 且匹配已检查 Mali build-id 时，
通过驱动原有进程内控制位跳过 Android loader 私有数据检查。
不写系统属性、不改 Vulkan 对象头/pNext；默认关闭，具体限制见
[ICD 说明](hybris/vulkan/icd/README.md#inspected-mali-mmud-workaround-opt-in)。
runner 的 `--icd-mali-loader-quirk` 只用于 ICD 及其捕获命令，并记录实际调整。
构建后红米 `20260907T065438-27c734a0` 为 96 PASS / 4 UNSUPPORTED / 1 CRASH；
X300 `20260907T065437-096b67ab` 为 93 PASS / 4 UNSUPPORTED / 2 CRASH / 2 FAIL。
普通/动态/large/staged widget、相应 validation 和两种 capture/replay 均通过，
caps/caps2 记录值不变。关闭选项的同库负对照 `20260907T065418-7b030d21`
仍复现原崩溃。native-groups、ICD core11、template 和 template-validation 尚未解决；
其余固件、驱动内部优化行为、任意 layer/应用、WSI 均未据此验收。

2026-09-07 ICD 版本协商修复：删除 private manifest 写死的 API 1.0，新增
`icd-version` 独立探针，直接查询 adapter/HAL 并用实测版本生成 driver.json。
Khronos loader 在 JSON <1.1 时不会查询 driver version，并向 HAL 传入 1.0；
此前“interface 5 会自动纠正”判断有误，旧 Adreno 通过记录不证明 HAL 收到
应用请求的 1.1。该错误导致 X300 core11 入口缺失及 template 路径失败。
现 X300 实测 1.3.305、红米 1.1.128；发现失败会停止依赖用例，缺失 HAL
负对照 `20260907T070107-1a7bcb32` 已验证。
开启上述 Mali 选项后，完整 X300 `20260907T070031-042f6354` 与红米
`20260907T070032-2711e780` 均为 97 PASS / 4 UNSUPPORTED / 1 CRASH，唯一
崩溃为 native-groups；core11、template、对应 validation、两种 capture/replay
均通过。默认关闭 Mali 选项时原管线问题仍存在。该 headless 结果不关闭
任意应用、WSI/present、一帧窗口捕获或其他驱动版本的验收门槛。


2026-09-07 dynamic rendering／synchronization2 定向验收：独立 render_path
模块复用固定 widget，分别用 core API 1.3 和 API 1.1＋KHR 扩展启用两个
feature，实际执行 Begin/EndRendering、PipelineBarrier2、QueueSubmit2、
fence 与中心像素读回；检查两种已知 UBO 绑定。覆盖 GIPA/GDPA、ELF/link，
标准 ICD 的两个查询路径另启用 VVL＋SyncVal。此处是动态渲染，已有
capture-dynamic 则是动态 UBO 偏移，两者不可混作捕获覆盖。
完整 X300 `20260907T071924-d081b4de`（开启限定 Mali 选项）为
110 PASS / 7 UNSUPPORTED / 3 CRASH；查询路径及 core ELF/link 通过，
frontend KHR ELF/link 在缺失 vkCmdPipelineBarrier2KHR 下游导出处中止，
另一个崩溃仍为 native-groups。尝试转 core ELF trampoline 仍崩溃，已撤掉；
不能仅凭 registry 同签名 alias 宣称直接调用安全。替代前端尚需对象作用域
的真实 dispatch，G02 保持未完成。
完整红米 `20260907T071925-53a698f8` 为 97 PASS / 22 UNSUPPORTED /
1 CRASH；新增 18 项均受实际 API/extension 限制，未伪装通过。两台已有
widget 校验和两种捕获回放均通过。此批不覆盖 multiview、depth/resolve、
secondary command buffer、多队列/semaphore、non-coherent 内存或 WSI；
不关闭 G09、G04/G11 的窗口/应用验收。


2026-09-07 KHR 直接绘制修复：替代前端新增独立 render_dispatch.c，登记
实际 device、queue、command pool 和 command buffer 的归属；device 创建时
分别保存 Begin/EndRendering、PipelineBarrier2、QueueSubmit2 的 core/KHR
GDPA 结果。ELF/link 和可用的 GIPA/GDPA 包装共用对象分发，不读取/修改
Android dispatch header，不把 KHR 名称强行转 core trampoline。销毁 pool
时回收其隐式释放的 command buffer 记录；显式 free 和 device 销毁同步移除
对应记录。Vulkan 导出名称仍为 643 个。
完整 X300 `20260907T072903-d60ecde5` 为 114 PASS / 7 UNSUPPORTED /
1 CRASH；红米 `20260907T072904-613cf66c` 为 98 PASS / 23 UNSUPPORTED /
1 CRASH，均只剩 native-groups。此前 X300 KHR ELF/link 两处中止已修复。
X300 ICD 仍需显式 Mali 选项；红米渲染新能力仍未支持，未据此提高声明版本。
新增 render-owners 在 X300 验证两个 device 同时存活、queue2、六轮 buffer
free/reallocate、pool reset/隐式释放及 Submit2/fence。command-alloc 在两台
验证 pool 分配失败及 command buffer 批次部分分配回滚，返回 -1、live 增量
为 0；非零输出哨兵补验 `20260907T073033-e5e3003d`／
`20260907T073034-948a7fc7` 确认全清 NULL、恢复后的最终 callback live=0。
驱动/分配回调不在 registry mutex 内执行；metadata 使用相应 device/pool
分配器。已有并发 device 生命周期、VVL/SyncVal、两种捕获回放通过。
尚未证明多 GPU、并发 command recording、protected queue、secondary CB、
任意驱动分配失败或线性 registry 查找的竞争开销；外部同步仍由 Vulkan
调用者负责，绕过前端创建的对象不在登记契约内。G02 的其余入口/完整资源
状态和 G09 的同步语义、WSI/真实应用门槛仍未关闭。


2026-09-07 timeline semaphore 入口与 wait-before-signal：独立
`timeline_dispatch.c` 按传入的已登记 VkDevice 解析 GetSemaphoreCounterValue、
WaitSemaphores、SignalSemaphore 的 core/KHR 名称，查询与实际调用均在
registry mutex 外执行。旧前端 `20260907T073632-06569d89` 的 KHR ELF/link
中止已修复；不做 timeline 模拟、不提升声明能力，导出仍为 643 个。
探针分别请求 API 1.2 或 API 1.1＋KHR，按 feature 门控，覆盖 GIPA/GDPA/
ELF/link。跨线程 host wait/signal 后执行四轮 queue wait-before-host-signal：
未满足时 host wait 和 fence 均为 TIMEOUT，主机 signal 后 timeline/fence
完成，counter 精确为 3/5/7/9；复用 fence，不靠 queue/device wait-idle。
最终 X300 `20260907T074051-325fe194` 为 135 PASS / 10 UNSUPPORTED /
1 CRASH；红米 `20260907T074052-c6dc686b` 为 98 PASS / 47 UNSUPPORTED /
1 CRASH，剩余均为 native-groups。X300 ICD 继续使用限定 Mali 选项；其
core/KHR timeline 的 VVL＋SyncVal 均 errors=0，原有捕获回放也通过。
红米新增 24 项因 API/extension 不足记为 UNSUPPORTED。
当前提交没有 command buffer，只验证 semaphore 协议；多队列、实际资源
内存可见性、WAIT_ANY、多 semaphore、导入导出、并发 signal 单调顺序和
有未完成工作时的销毁仍未覆盖，不能据此关闭 G09 或全部 G02 alias。


2026-09-07 首个实际 Wayland 窗口：新增独立 [tests/wsi](tests/wsi/README.md)，
在现有可 run-as 的 Ardesk compositor UID 中运行替代 Vulkan 前端。窗口
320×240，八帧交替清绿/红，逐像素读回 swapchain 图像后 present，记录真实
image index 和 frame callback；截图检查同一完整窗口区域的颜色转换。
X300 初次 `20260907T075124-f24a7774` 因运行包缺 libwayland-egl.so.1 中止；
构建现改为收集全部已安装 ELF（含平台插件）的传递 DT_NEEDED 闭包，缺失
依赖会在构建阶段失败。17 个运行库包含 Wayland EGL；不再携带未依赖的
libbsd/libmd，解释器统一按 cross-sysroot 优先选取，哈希变化已做全量回归。
最终 X300 `20260907T080229-0e2a8fd6` PASS：八帧读回/提交/回调完成，屏幕
[1219,501,1539,741] 的 76,800 像素从绿变红。PNG 内嵌 Display P3，依据
实际 ICC 转换的预期编码值为绿(117,251,76)、红(234,51,35)，逐像素精确匹配；
没有用“看起来有窗口”或 frame callback 代替图像验收。保存完整首尾读回、
实际截图、checker/profile/ELF/APK 哈希、映射和 70 项 Android 库哈希。
红米 `20260907T080319-6c3d459b` 当前 compositor 无 android_wlegl，故为
UNSUPPORTED；未安装或替换 APK。runner 使用独立目录并仅清理自身进程/文件。
新运行库下完整 X300 `20260907T080304-6c3bd550` 为
135 PASS / 10 UNSUPPORTED / 1 CRASH；红米 `20260907T080125-f9ffdff6` 为
98 PASS / 47 UNSUPPORTED / 1 CRASH，剩余均 native-groups；原有校验与两种
离屏捕获回放通过。X300 ICD 仍使用限定 Mali 选项，此窗口走替代前端。
本次只有固定单窗口和两个实测颜色，不证明 resize/minimize/out-of-date、
多窗口/多 surface generation、release-fence 退休、FD 无泄漏或任意图像。
frame callback 不等于 buffer release；截图有显式停顿，不能作无阻塞性能
证据。标准 ICD WSI、带 present 的 capture/replay、真实应用故障闭环仍未
完成，G04/G06/G11/G12 不据此关闭。


2026-09-07 Wayland surface 并发与 FD 回归：surface map 的插入、查找和移除
现由同一 mutex 保护；销毁一次性取走记录，释放锁后才调用 driver/Wayland。
同一 surface 的使用/销毁仍要求调用方遵守 Vulkan 外部同步，未把非法并发
或 stale handle 变成合法。额外生命周期探针独立在 surface_lifecycle.c，
四线程各八轮，每轮创建四个独立 wl_surface/VkSurfaceKHR，barrier 保持
16 个同时存活，再各自逆序销毁；创建线程失败会释放并 join 已启动线程。
X300 `20260907T081226-add42893` 共 128 对成功，预热后 client FD 为 7→7，
随后的八帧窗口与 76,800 像素屏幕比对通过。旧库对照
`20260907T081105-efeb0f17` 也通过，故仅称代码识别出的无锁 map 竞争修复，
不虚报动态复现；Wayland 平台 67 个导出名称不变。
红米 `20260907T081226-1182df69` 当前 compositor 缺 android_wlegl，仍为
UNSUPPORTED。新库完整 X300 `20260907T081302-cd38e27b` 为
135 PASS / 10 UNSUPPORTED / 1 CRASH；红米 `20260907T081302-0f50a7a0` 为
98 PASS / 47 UNSUPPORTED / 1 CRASH，均只剩 native-groups；校验与两种
捕获回放通过。此 surface 压力部分无 swapchain，不证明多窗口绘制、
compositor FD 无泄漏、heap 回收、resize/out-of-date 或 release-fence 退休。
G03/G11 及窗口 capture/真实应用门槛继续保持未完成。


2026-09-07 Wayland discovery 失败路径：缺少 android_wlegl 时不再在 sync
callback 中 abort；私有队列 roundtrip 完成后返回 VK_ERROR_UNKNOWN(-13)，
并清理已创建的 registry/wrapper/queue。临时 wl_egl_window 也会在后端
surface 创建失败时销毁。旧库红米 `20260907T081905-2e8ed7fc` 实测
CRASH 134；最终 `20260907T082144-82103b66` 八次均返回 -13 并正常清理，
拒绝路径 PASS，窗口仍 UNSUPPORTED，不能据此声称红米 WSI 可用。
X300 `20260907T082144-8ee218a3` 的 128 对并发 surface、client FD 7→7、
八帧读回与 76,800 像素屏幕转换均 PASS。67 个平台导出名称不变。
分配失败、display 断连、后端创建失败只检查代码，未注入验证；无 heap
回收、resize、release-fence 或标准 ICD 窗口证明，G03/G11 继续开放。
最终构建完整回归：X300 `20260907T082214-6907c912` 为
135 PASS / 10 UNSUPPORTED / 1 CRASH；红米 `20260907T082214-648d0d5c`
为 98 PASS / 47 UNSUPPORTED / 1 CRASH，均仅 native-groups 崩溃。
校验、SyncVal 与两种离屏 capture/replay 均通过，X300 ICD 仍用限定 Mali
选项；此次窗口仍走替代前端。


2026-09-07 同窗口 resize / oldSwapchain：旧前端
`20260907T082626-c5caa97e` 第二次 CreateSwapchainKHR 超时 142；调试构建
`20260907T083148-f82d7506` 定位 native-window disconnect 空操作导致旧
显示 buffer 占槽，新链取完三个后卡在第四次 dequeue。Vulkan Wayland
现断开旧 producer 池，保留呈现中的 buffer 直到 wl_buffer.release，并使
其 release 不增加新池可用计数。新增 C++ virtual hook / 私有 helper 签名
要求完整重建平台包，其他平台默认行为不变，Vulkan 643 导出名称不变。
X300 发布构建 `20260907T084008-36e56a41` 同窗口三个尺寸
320×240→448×288→256×192，24 帧及三对完整屏幕颜色转换通过，
合计 254,976 像素。用旧 320×240 截图替换大尺寸截图时 checker 正确拒绝。
调试构建 `20260907T084042-7f9dc528` 同样通过，观测每次断开保留一个
旧显示 buffer，随后新呈现触发它的退休 release。红米
`20260907T083436-db561ef3` 八次缺协议拒绝通过，窗口仍 UNSUPPORTED。
所查 compositor 源码的 GPU binding 表仅八个 PID 槽，客户端退出后不回收；
多进程迭代后旧/新前端都曾读回正确但屏幕透明。08:39 确认只有测试
xterm 后重启测试桌面清空该表，上述最终窗口结果基于新会话，未安装 APK。
现象与表耗尽一致，但未插桩测量运行 APK 的确切槽数。此依赖回收缺口与
独立测试 compositor 仍待修；不能靠重启声称泄漏门槛通过。
重建边界使用 wait-idle，未证明未完成 GPU 工作时重建、失败恢复、
compositor 拖动/out-of-date、release-fence、swapchain FD 无泄漏、标准
ICD WSI 或 present capture/replay；G03/G11/G12 继续开放。
最终发布构建完整回归：X300 `20260907T084136-0606339b` 为
135 PASS / 10 UNSUPPORTED / 1 CRASH；红米 `20260907T084044-c173adc0`
为 98 PASS / 47 UNSUPPORTED / 1 CRASH，均仅 native-groups 崩溃。
VVL、SyncVal 与两种离屏 capture/replay 均通过，X300 ICD 仍用限定 Mali
选项，不能据此覆盖标准 ICD 的窗口路径。


2026-09-07 开发迭代：新增 --incremental，以内容同步 C/C++/汇编输入并保留
成功构建缓存；头文件/规则/配置/文件集合变化保守全建。inputs 与编译目录
分开，install/runtime 每次重建，清单标注模式，同目录并发构建被锁拒绝。
调试冷构建 48.475 秒，无改动 9.230 秒；两个 Wayland 源文件保留旧 mtime
改动仍触发三个对象重编，11.936 秒完成，仅两个平台插件 ELF 改变。
该产物 X300 `20260907T085347-860b2245` 三尺寸窗口通过；精简 trace 保留
断开/退休释放，日志由先前 153,855 行降至本次 2,081 行，非通用大小上限。
最终发布冷建 46.868 秒、无改动增量 9.437 秒，75 条部署 ELF 哈希一致。
X300 `20260907T085914-d8c8a645` 24 帧与 254,976 屏幕像素通过；红米
`20260907T085914-8f94a259` 拒绝路径通过、窗口仍 UNSUPPORTED。
配置/脚本变化触发全建、并发拒绝和复用自身头文件快照均实测；头文件增删
及失败重建分支只审查未逐一完整构建注入。独立测试 compositor、双端失败
现场自动收集仍待完成，本地缓存也不替代干净构建验收，整体验收继续开放。
最终全量回归 X300 `20260907T090014-cd3e02ef` 为
135 PASS / 10 UNSUPPORTED / 1 CRASH；红米 `20260907T090014-d22fcaa8` 为
98 PASS / 47 UNSUPPORTED / 1 CRASH，仍均只有 native-groups 崩溃。
VVL/SyncVal 与两种离屏捕获回放通过，X300 ICD 仍使用限定 Mali 选项。

2026-09-07 双端失败现场：WSI runner 新增独立 diagnostics.py，滚动保留
最多 512 KiB compositor PID 日志；十秒无客户端输出时、或 host timeout
终止前，采集带 PID/cwd 核对的客户端状态、maps、FD、线程等待点及
compositor 状态和截图。文本每份最多 256 KiB，采集命令有五秒期限，
错误/截断/collector hash 单独记录。旧库 X300
`20260907T094746-e0dc168e` 实测 TIMEOUT 124，主线程 do_sys_poll，客户端
70,618 字节、compositor 155,310 字节、日志 17,596 字节，均未截断，
终止前后截图存在。最终新库 X300 `20260907T094825-24bddd09` 三尺寸
24 帧及 254,976 像素通过；红米 `20260907T094825-0f077796` 拒绝路径
通过，窗口仍 UNSUPPORTED。日志 reader 均退出，运行目录清理完成。
库包增量实际构建 9.885 秒，本批仅 host 诊断变化，不冒称新的完整离屏
回归。中断期间测试桌面已退出，重新启动后验证，未安装 APK。日志首条
可能早于运行、PID 重启不跟随、进程退出后无法追补现场；无 GPU/backtrace/
release-fence 证据。独立 compositor 与整体验收仍未完成。

2026-09-07 独立 compositor 开发夹具：新增 tests/wsi/compositor，单独
包名 io.taowen.hybriswsitest、UID、Activity Surface、进程和 runtime/socket，
只运行 anlabwc，不启动 rootfs/xterm/Gladio/Vortek。JNI/Java 实际编译，
后端及 DT_NEEDED 闭包、xkb 从明确指定的 APK 导入并记录 SHA256，不冒称
后端源码可重现构建。两机经现有 install-apk.sh 安装独立包，未替换 Ardesk。
每次 wrapper 只停止专用测试包，等待新 socket 和单一 PID 后执行既有
探针，最后清空测试进程并记录前后 PID。Surface 销毁即退出测试进程，
固定横屏；不声称后台运行或 Surface 重建恢复。
最终 X300 probe `20260907T095812-bf739cc9`、红米
`20260907T095812-54cb3f53` 均三尺寸 24 帧及 254,976 屏幕像素 PASS。
两机开始时均无旧 PID，使用 29659/2198，结束无残留；compositor 日志
16,777/16,885 字节未截断，reader 退出。此前两轮独立 wrapper 也通过。
红米的独立后端有 android_wlegl，原 Ardesk 旧 endpoint 不支持的结论仍
限于原 APK。后端八客户端绑定表长期回收缺口未修，文件/字体缓存仍跨轮
存在；这是进程/native 状态隔离，不是全文件系统或 Android GPU 隔离。
本批新 APK/夹具已实际构建验证，libhybris 本体未变，未重复冒称完整离屏
回归；标准 ICD WSI、present 捕获回放、真实应用及 G03/G11/G12 仍未关闭。

2026-09-07 长期 compositor 的连续客户端反例：wrapper 新增 --repeat N，
一次启动 compositor 后顺序执行独立客户端，前后验证 PID＋/proc/stat 启动
时间不变，保存每轮客户端 PID、compositor FD 列表及完整像素结果；任一
非 PASS（包括 UNSUPPORTED）即失败停止，不能把部分通过当成整轮通过。
WSI C 探针已重新构建，新增 getpid 证据；未改生产库、未新增单元测试。
X300 `20260907T100827-ad521e8a` 与红米 `20260907T100827-f9d18538`
均在同一 compositor 中运行九个不同 PID：前八次各三尺寸 24 帧、254,976
屏幕像素通过，第九次虽有 24 帧精确读回与 callback，却无屏幕绿红转换，
明确 FAIL，未执行第十次。两机 compositor PID/starttime 全程不变，
结束均无测试进程残留；此前一轮也复现相同边界。X300 每轮后 FD 为
200/202/205/208/211/214/217/220/220；红米
172/166/168/170/172/174/176/178/178，初始分别 177/151。
这些观测与源码八槽 AHB 绑定不回收一致，但未直接插桩运行二进制的槽内容，
也不能把全部 FD 增长归给单一对象。并发第二个 wrapper 实测被同设备锁
拒绝，原测试进程保持。此批建立可重复失败门槛，尚未修复依赖后端回收；
G03/G11、标准 ICD WSI、present 捕获和实际应用仍开放。

同批默认单次新进程恢复验证 X300 `20260907T101154-536a15c5`、红米
`20260907T101154-17607f09` 均三尺寸 24 帧、254,976 屏幕像素 PASS，
结束无测试 PID；恢复不等于回收修复，生产库未变，未重复声称完整离屏回归。

2026-09-07 Wayland AHB 绑定回收修复：依赖 anlabwc `25a829e9` 将
android_wlegl 绑定归属到真实 wlr_surface，销毁时移除槽并释放 AHB，立即
同步 Android overlay 表以释放其引用。原 PID 入口不变；八个同时存活槽的
容量、多窗口/subsurface、异常退出、X11 外部提交及完整 fence 生命周期
未覆盖。NDK 实际重编两份 C 文件并链接，独立 APK 使用 --backend-library
导入新后端并记录 SHA256；两机只更新专用测试包，未替换原 Ardesk APK。
最终 X300 `20260907T102323-39154d6d`、红米
`20260907T101850-d5ce6967` 各在同一 compositor PID/starttime 中连续通过
十二个不同客户端，各 288 帧、3,059,712 屏幕像素。每个客户端销毁后记录
binding active=0；预热后的每轮 FD 分别保持 199/164，结束无测试进程。
旧后端两机均第九客户端失败且 FD 逐轮增长，见上一条，故这次有直接前后
反例。但 X300 首次 `20260907T101850-87d2827b` 的第十一客户端仍保留为
FAIL：充电通知可见时顶部 39 行有单通道差 1，完整窗口及读回存在；通知
是否导致偏差未独立隔离。未放宽 checker，最终重跑通过不抹去该结果。
修复源码/构建库及所有结果索引见 tests/wsi/compositor/README.md。
这关闭已复现的正常 surface 销毁后绑定耗尽，不是全部 G03/G11 或新增
桌面 OpenGL/Vulkan 特性；标准 ICD WSI、present capture、真实应用仍开放。

2026-09-07 首个实际桌面 GL 前端：tests/desktop-gl 从固定 Mesa
`1cb7f0a1c9a5438045f89ad4aa83eda8fbafa09e` 构建仅 Zink 的 Gallium 和
Mesa EGL，标准 glibc Vulkan loader → hybris ICD → vendor HAL。
X300 `20260907T104023-a4d52adb`（3.2 core）与
`20260907T104042-984ffd94`（3.2 compatibility）均创建成功，实际报告
Mesa/Zink、Mali-G1-Ultra、GL 3.2、GLSL 1.50；GLSL 编译/link、VAO、
gl_VertexID 三角形和 gl_FragCoord 两色绘制通过，C/host 各校验全部
256 像素且 GL error=0。映射含实际 Mesa、标准 loader、hybris ICD 和
Mali HAL；继续显式使用既有、限定 build-ID 的 Mali loader 选项。
3.3 core `20260907T104053-34894e46` 返回 EGL_BAD_MATCH，明确
UNSUPPORTED；红米 `20260907T104023-86f66a86` EGL 初始化失败，记 FAIL。
后续生成代码复核纠正了此前的诊断：生成器已识别 KHR 名称；首次 3.3
阻塞项实际是打包顶点格式。KHR 属性结构还有独立缺陷，见后续修复记录。
红米缺 timeline/maintenance5 等该版本需求；尚未隔离 EGL 失败的单一原因。
构建源码、编译器/image、ELF 哈希、maps、image 和失败记录见新目录 README；
未覆盖整套 GL 3.2、固定管线、UBO/SSBO/FBO/纹理/sRGB、GLX、窗口呈现、
Zink validation 或 Blender。没有 GL/GLSL version override；不是 Gladio/
Vortek 同等特性声明。G10 有首个实际功能进展，整体验收保持开放。

2026-09-07 桌面 GL 打包顶点与实例化修复：实际 GL extension 枚举
`20260907T104507-85166d8c` 证明实例化属性已支持，缺的是
ARB_vertex_type_2_10_10_10_rev。纠正此前“只识别 EXT 名称”的错误诊断：
生成器已有 KHR 名称支持。Mesa 依赖 `2e3d35e` 在现有 u_vbuf 转换器中
加入八种 10/10/10/2 pipe 格式，缺失的原生取数转为四分量 float，GL
能力查询检查同一目标格式；没有改变 Vulkan 格式宣告。
不同实例数据揭露第二个缺陷 `20260907T105012-3a49ec45`：除数 1 通过，
除数 2 六种组合各 128 像素错误。诊断确认 EXT 属性 sType 查询 KHR-only
驱动得到 max=0，动态输入最终按 1 取数。`37f170c` 改为查询不同 sType
及大小的 KHR 属性结构，保留 EXT-only 与 Vulkan 1.4 的对应路径。
最终 X300 `20260907T105351-1b2678ab`（请求 core 3.3）、
`20260907T105419-ad7a9372`（请求 compatibility 3.2）均通过十二次不同
打包输入/除数组合及原绘制，每次 256 像素精确、GL error=0；每次先清蓝，
避免旧帧假通过。实际自报 GL 4.4 / GLSL 4.40 不等于完整 4.4 验收。
红米 `20260907T105351-89b27f4b` 仍 EGL 初始化 FAIL，未声称 EXT-only
路径真机绘制通过。构建、哈希、反例与范围见 tests/desktop-gl/README.md；
CPU 转换性能、边界值、indexed/base-instance/indirect、Zink validation、
桌面窗口及 Blender 仍未覆盖，G07/G10/G13 未全面关闭。

2026-09-07 G10 后续进展：Mesa GLX 已接入并在 Mali 验证 core/compat
pbuffer 绘制；实际 Blender 4.3.2 在 GLX 下因顶点 SSBO 上限 0 而拒绝
启动（片元/计算为 16，应用要求每阶段至少 12），不能算应用验收通过。
Mesa 依赖 `6bec718` 随后加入显式开发开关下的自动程序顶点 NIR→计算→
只读顶点回放路径，验证 272 字节 UBO、默认 uniform、非零 first vertex/
base instance 及三阶段范围切换。原生、EGL core/compat、GLX 图像一致；
标准 validation 确认 SyncVal 已启用且零错误，60 份 SPIR-V 校验通过。
组合计算/自动绘制发现的解绑泄漏及已删除 SSBO 过早释放均已修复。
该路径尚排除顶点属性、纹理、clip/cull、索引/间接、多阶段等情况，按 draw
创建内部管线/缓冲区，未完成通用模拟或性能工作；顶点 SSBO 宣告仍为 0。
原始失败、最终固定构建记录和具体范围见 tests/desktop-gl/README.md。
G08/G10 与 Blender、完整窗口呈现、跨设备验收均继续开放。


2026-09-07 自动顶点属性取数：Mesa `8986040` 将原 VBO 绑定到计算
SSBO，复用 Mesa 格式解码，支持本批 32 位对齐的 offset/stride、
first vertex 与 base-instance/除数寻址。新增独立绘制覆盖 float32、
归一化 UBYTE、整数 SHORT、半精度及缺失分量默认值；非零 first=7、
base-instance=5、除数 1/2 在 EGL core/compat 与 GLX 计算路径均逐像素
PASS。组合原打包、UBO、显式计算/删除用例也通过，SyncVal 已启用且零
错误，三轮共 192 SPIR-V 校验通过。未提高任何 GL/Vulkan 能力。
原生对照 `20260907T122644-031d5f2e` 在 base-instance=5/除数=2 时
256 像素全部失败；无显式计算负载的 `20260907T122849-e644af24` 独立
复现，GL error=0，保留为 FAIL。尚未隔离 Zink 状态或 vendor 取数原因，
不得声称原生全面一致。具体记录见 tests/desktop-gl/README.md；未对齐、
64 位、一般 SSBO 输入、性能、Blender 及整项 G07/G08/G10/G13 仍开放。


2026-09-07 原生 base-instance/除数反例修复：诊断
`20260907T123701-fe4e1c58` 确认 Mali 返回
supportsNonZeroFirstInstance=false，旧 Zink 仅复制最大除数，丢弃此限制；
动态输入除数确为 2，firstInstance=5 下实际取数索引 2/3/3/4，非预期
5/5/6/6。Mesa `8bb94e2` 保留能力标志，通过独立 helper 重定位实例 VBO
偏移并使用 firstInstance=0，以 push constant 保留应用 BaseInstance。
支持本批直接/索引与已解码间接绘制，未读回顶点数据；间接参数需同步读取，
性能未验收。`6392c27` 合并 Gallium 参数解码并修正 padding stride、
映射范围和 count 上限。多绘制还复现了 DrawID 改变未触发管线更新的
128 像素失败，已修正；原始失败记录保留。
固定构建后的六轮（原生/计算 × EGL core/compat/GLX）全部通过：八阶段
属性矩阵、原打包、显式计算/删除和 UBO 回归，15 张图像跨轮一致；SyncVal
已启用且零错误，291 份 SPIR-V 校验通过。结果索引、shader 内
BaseInstance/BaseVertex/DrawID 检查和 GPU 写入间接命令/count 的范围见
桌面 GL README。只关闭上述已复现失败；robust 越界取数、全部阶段/拓扑、
EXT-only/Vulkan 1.4 分支、性能、顶点 SSBO 和 Blender 仍未验收，未提高
任何能力或放宽 G07/G08/G10/G13 整项门槛。
