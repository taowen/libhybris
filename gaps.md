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
| G02 / P0 | 全入口 dispatch、core/KHR alias、每 instance/device 的真实函数表；避免包装绕过/递归/NULL branch | 从 vk.xml 固定版本生成覆盖表；同一测试经 link/dlsym/GIPA/GDPA；BeginRendering/KHR、Submit2/KHR 等按启用能力测试；未支持符号符合规范，不假成功。**部分落地**：普通 GIPA/GDPA 查询保留 backend 对 instance/device 的解析结果，只替换必要前端/WSI 包装；新增作用域/未启用扩展的负例测试，未解析的直接导出调用有错误信息。已从固定 Vulkan-Headers v1.4.309 registry 生成 726 个命令的 scope/alias/provider 清单；真机记录 dlsym/GIPA(NULL)/GIPA(instance)/GDPA，检查 137 个 core 1.0 必需入口及非 global/non-device 的禁止作用域。相同 fill/fence/readback 已经 link/dlsym/GIPA/GDPA 实际执行，memory2 的 core 1.1/KHR 已启用并验证。swapchain 包装已按当前 device 的 GDPA 解析并在缺失时先拒绝，Wayland 创建/销毁按当前 instance 查询（仅构建验证）；未启用扩展的直接导出负例不再返回成功。可选标准 ICD 已保存 instance resolver/destructor 与唯一 generation，并由查询/销毁入口使用；替代 libvulkan 前端与 device/resource 尚无完整兼容状态表；dynamic rendering/synchronization2 的语义与全部命令执行尚未覆盖 |
| G03 / P0 | 多线程/多 context/多 device 与对象生命周期 | 并行 create/destroy、二次 init/dlopen、回调、线程 TLS、handle 重用有回归；对象 state 按 generation 识别，不能用进程全局单一 current device。**部分落地**：谁持有谁：glibc 持有 `libhybris-common`（`DF_1_NODELETE`）至进程结束；common 持有 Android linker plugin，不 `dlclose` 它；frontend 持有自身 glibc 引用；vendor 对象由 `android_dlopen` 调用方持有，frontend 关闭不回收它们。允许的关闭顺序：销毁 Vulkan device/instance，丢掉 frontend 引用，进程退出。`ENSURE_LINKER_IS_LOADED()` 改为 `pthread_once`：审查现有 bundled linker 初始化路径未发现重入公开 `android_*` wrapper，后续新增回调必须继续遵守该约束；`_hybris_hook_dlerror` 改为直接调 `_android_dlerror`，避免 once 期间重入。baseline `life` 覆盖双 device、destroy/recreate、二次 dlopen、两线程 create/destroy，但不覆盖首次进入。`init` 覆盖两线程同时第一次 `android_dlopen`；`tls` 覆盖工作线程完成 Vulkan 创建/销毁后主线程关闭 frontend、再让工作线程退出；已用 bionic C++ DSO 的 emulated TLS 和原生 TLSDESC 两种产物观测初值与析构；原生变体曾在 glibc 工作线程首次读到 0，现由 Q linker 静态 TLSDESC resolver 先初始化线程并重放 .tdata 修复。关闭主引用后，析构在所属线程恰好执行一次、值正确，三轮均通过；不证明 compat allocation 清理、IE TLS/signal 场景或静态槽回收。`20260906T233143-5f5b13ed`：hybris `init`/`tls`/`unload` PASS；native `init` 无对应实现，不再调度；hybris common 加载失败报 FAIL。注入第二次 pthread_create 失败后能释放 barrier、join 已启动线程并退出 2。`dlclose` 返回 0 不等于已解除映射；进程退出 0 不等于资源已回收；重复创建成功不等于 generation 管理。可选 ICD 新增 instance generation 表，四线程 16 次创建/销毁记录在两台设备配对且无遗留；探针进一步要求每轮四个 instance 同时存活；另有应用 allocator 三轮创建/查询/销毁无未释放分配、拒绝分配返回 OUT_OF_HOST_MEMORY 的三路径对照；标准路径失败可能在 loader 层发生；另有直接 ICD 探针验证状态记录首笔分配失败仅调用一次 allocator、恢复后三轮生命周期无遗留，仍不代表每个 HAL 分配点或 allocator 内部锁交互；分配回调按规范不得调用 Vulkan 命令。以上是 instance 级证据，不覆盖 device/resource，尚无句柄实际复用的证明。已补 GLES 两个不共享 context 的 buffer/clear state 隔离、pbuffer 精确像素、主线程→工作线程→主线程迁移和三轮销毁重建，两台设备 native/hybris 均通过；补充 `vk-init` 四线程同时首次枚举/创建并各自完成四轮 instance 查询/销毁，平台初始化与全局入口解析改为 pthread_once；null 平台真机验证，Wayland 仅构建验证。静态 Android mutex 改为受锁保护的唯一后端指针发布，兼容四字节对齐；bionic DSO 的 32 把新锁、四线程首次竞争在两台设备通过，旧 common 负对照超时。已补 rwlock 唯一后端发布，32 把新锁四线程写互斥及同时读持锁/try-write 拒绝；cond 的首次指针发布也已串行化，并补 32 个新条件变量的 timedwait/signal/broadcast 正常唤醒探针；旧版本次亦通过，未复现丢失唤醒。缺少 glibc /dev/shm 时 allocator/translation 不再空指针崩溃，三种共享同步对象初始化返回 ENOMEM；这不代表正常共享同步已支持。两个 monotonic cond 别名已修复为显式 CLOCK_MONOTONIC，100ms deadline 不再立即超时；relative 入口改为 monotonic 并拒绝非法纳秒、负秒数和溢出 deadline，独立探针验证正常等待及 EINVAL。正常共享同步、其他 cond 时钟/销毁语义及 rwlock 公平性/超时语义仍未验证。已补共享 GLES2 context 的 buffer 大小、纹理精确像素、binding 隔离及创建者 context 销毁后继续访问，三轮两台 native/hybris 均通过。已补两个不共享 context 同时在各自线程 current 后执行独立 clear/readback、join 后迁回主线程复查。独立 context 已补蓝/黄片元程序的并发主机线程 draw 与精确像素，迁移后不重绑程序/顶点属性。没有 generation 对象表或 GPU 执行重叠证明 |
| G04 / P0 | 标准 loader/layer/tool 接入 | 一个已知非法小测试被 validation 捕获；一个合法小测试零新增错误；完成一帧 capture/replay 且像素匹配，再扩大到应用。**部分落地**：可选 ICD 直接接 vendor HAL，标准 glibc loader 的 8 项 headless 用例在两台设备通过；不改写 dispatch header、不删创建链。已用标准 glibc VVL 1.4.309.0 验证合法 instance/device/buffer 生命周期零 ERROR，以及零长度 buffer 精确 VUID 并阻止进入 vendor；两台设备通过。固定 widget 的上传/绘制/读回/销毁已启用 SyncVal，两种 binding 均预期像素且零 ERROR。固定 GFXReconstruct 已完成离屏 widget 捕获/回放：正确/错误 binding 各 16×16 RGBA8，未捕获、捕获、回放三份完整图像逐字节匹配。无 present frame，不能据此关闭一帧 WSI/应用捕获门槛；WSI 未实现。此前手写 validation chain 的结果仍已撤回 |
| G05 / P0 | 能力宣告与模拟实现脱节 | features/features2、properties/limits、extensions、format/image-format query 与 CreateDevice enable 路径一致；保留原始/有效能力差异及原因；不通过删整个 pNext 重试。**部分落地**：baseline `caps` 核对未广告 feature / 未知扩展的精确拒绝错误，并检查未启用扩展的 GDPA 返回 NULL；已保存同设备 native/hybris/ICD 的 325 个具名查询值与差集：174 个 core feature/limit/sparse 值、设备标识、设备扩展及十种格式查询。两台设备前端无差异，直接 HAL ICD 有六项 buffer/present 扩展差异；不能按全栈等价解释。已补五个 core 1.1 features2 结构、55 个 core 字段一致性、同链设备启用及 false float64 的精确拒绝；三条路径两台设备通过。已补六个 core 1.1 properties2 结构的整链/逐结构对照及旧 properties 具名字段比较；两台设备三路径保存的 198 个 features2/properties2 值一致。其余扩展查询链、全部格式创建验证和兼容变换原因记录仍未完成 |
| G06 / P0 | 缺少 draw→资源→image 诊断链 | 用 Mali-shaped UBO 测试导出绑定/布局/内容/attachment 证据；人工注入错误 binding 后能定位首个错误 draw。**部分落地**：272B std140、12 vertices / 18 indices 的固定 fixture，正确 binding 像素为 `255,255,0,255`，注入 binding 为 `0,255,255,0`；已补 color→transfer→host 同步。已通过 GFXReconstruct 导出 draw-time descriptor 和完整 272B UBO、attachment 前后图，验证 set/buffer/range 与 update/bind 对应、attachment 与 copy 源同 image；两台设备固定负对照的首次像素分叉定位到 draw 60。另有 272B dynamic UBO 探针，以非零 descriptor base 和 dynamic offset 经 native/frontend/ICD 验证正确及替代数据的精确像素，ICD VVL/SyncVal 零错误；新增实际 1232B 合成 shader block，逐项检查 14 个非对称 mat4（array stride=64、column stride=16）、尾部 vec4/vec2 和 bool/int，普通/动态 descriptor 的两组数据均经三条路径得到预期像素，ICD VVL/SyncVal 零错误；尚非 Blender 完整 instanced widget 布局，多 dynamic binding、template、重录/重提交及动态捕获状态重建未覆盖。仍不能定位任意应用的首个错误 draw，捕获局部 ID 不是 runtime generation；无 WSI attachment lineage |
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
- G03/G05/G06 只有 headless probe，没有 generation 对象表或完整证据包。`unload` 现在能正常退出，是因为 hooks DSO 被钉住，不是因为 Android Vulkan 对象可回收。`init`/`tls` 检查并发首次 `android_dlopen` 得到完整结果，以及 frontend 关闭后工作线程能退出；另由 `tls-dtor` 观测两种 TLS 模型的实际 C++ 析构，`tls-bounds` 检查跨线程注册的初值补齐与本地修改保留。它们不证明映射消失、静态槽回收或所有 vendor TLS 析构正确。

## 8. 建议实施顺序与完成门槛

1. **建立可信基线（G01–G05）**：独立构建/运行、产物 manifest、统一 dispatch、loader/layer spike。门槛：小型合法 workload 完整通过；故意错误被诊断；原生与 passthrough 差异可解释。
2. **先补诊断（G06/G12）**：Mali widget UBO/readback 与 draw state、attachment lineage、shader dump。门槛：注入错误绑定/颜色转换后能指出首个失败阶段，不再只得到一张白屏截图。
3. **按证据增加兼容功能（G07–G09）**：优先 BC 常见子集、scaled vertex、已知 shader/driver quirk；每项 capability 精确门控、单独开关和 reference。移植前先跑参考 Vortek 算法对应小测试。
4. **桌面 GL 与 WSI（G10/G11）**：先确定目标 GL profile 和 Zink 差集；补 Vulkan X11、统一窗口提交。工具和测试仍独立，最后才接 ardesk 验证。
5. **扩大真实应用/设备回归（G13）**：在原故障设备重现并闭环两个 Blender 案例；Turnip 原 bug 是否修复与 hybris 兼容性分开结论。一个 Adreno 650 的通过不能替代 Mali/Adreno 830 的结果。

每个 workaround 必须包含：触发条件（vendor/device/driver/feature）、为何需要、转换语义、开启/关闭结果、回归用例、额外成本和撤销条件。原始 shader、capability、pNext 不可悄悄丢弃。
当前不宜承诺的结果：完整 OpenGL 4.x/Vulkan 1.3+、任意 app 都正确、所有 GPU 黑屏自动诊断根因、纯 libhybris 修复 Turnip 内核/编译器缺陷。先建立能定位与验证的兼容平台，再逐项扩大支持面。
