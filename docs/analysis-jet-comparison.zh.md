# ProtoTracer 深度分析 × Jet 对比研究报告

> **版本**: 1.0（研究稿）　|　**日期**: 2026-08-21　|　**研究目标**: 为 ProtoTracer-ESP32S3-Port 的未来优化寻找可落地的参考依据
>
> **参考对象**: [CubeCoders/Jet](https://github.com/CubeCoders/Jet)（克隆于 `reference/Jet`，commit `4db6aa1e`，2026-06-27，`main` 分支，浅克隆）
>
> **分析对象**: 本仓库（`src/main.cpp` 固件版本 `PROTOTRACER_FW_VERSION = "1.2.12"`，ESP32-S3 N8R8 主目标）
>
> **配套文档**: 本报告是对 `docs/optimization-plan.md`、`docs/render-acceleration-review.md` 的补充——既有文档写于 2026-05-31，彼时尚无 Jet 作为参照；本报告以 Jet 的实际实现验证既有优化方向，并补充既有计划未覆盖的新思路。**注意**：本次走读发现，2026-05-31 文档中记录的若干问题（MaterialAnimator 越界、邻居表 32 KB、`Animation<1>` 容量、boop 字符串拷贝等）在当前代码中**已修复**，详见 §3.9。

---

## 0. 摘要

ProtoTracer 与 Jet 是**同一硬件档次（ESP32-S3 级）、同一软件范式（纯 CPU 软件 3D 渲染）、不同实现路线**的两个项目：

| | ProtoTracer | Jet |
|---|---|---|
| 渲染范式 | **逐像素光线投射**（ray-casting）＋ QuadTree 加速 | **逐三角形扫描线光栅化**（scanline rasterization） |
| 数学 | 全 float（带 ESP-DSP 向量批处理） | 定点 Q10 为主、float 为辅（混合策略） |
| 输出设备 | HUB75 LED 矩阵（I2S DMA，RGB888）＋ M5 小屏 | RGB565 帧缓冲（SPI/并行屏，宿主自备驱动） |
| 深度排序 | 三角形平均深度 z-buffer（`averageDepth`） | `FAST_Z`（同样用平均深度）＋ painter 桶排序，可选真 z-buffer |
| 每帧堆分配 | 已基本消除（arena QuadTree、缓存复用） | 设计上零分配（静态 `std::vector` 容量预置） |
| 热路径虚调用 | 有（`Material::GetRGB` 纯虚函数逐像素调用） | 无（enum switch + 非虚方法） |
| 功能裁剪 | 散落的 `#define`（约 30 个） | 系统化的 `JetConfig.hpp`（约 40 个开关，全部编译期） |
| 性能量级 | 2048 像素面板，326 三角形面网格 | 480×320 下约 40,000 三角形/秒（flat） |

**核心结论**：Jet 是"直接三角形光栅化"路线在 ESP32-S3 上的**量产级参考实现**，它用实测数据证明了 `docs/optimization-plan.md` §7.3（Option C：直接固定三角形光栅化）方向的正确性与收益量级；同时 Jet 还提供了既有计划未覆盖的若干高价值技巧：增量边函数＋扫描线范围求解、painter 桶排序、混合定点/浮点数学、编译期特性矩阵、无虚函数着色器路径、以及桌面可移植测试基建。具体建议见 §6。

---

## 1. 研究背景与方法

### 1.1 研究目的

本仓库固件存在已知痛点（长跑内存碎片、渲染帧时偏高、无单元测试、功能裁剪粒度粗），团队希望借外部成熟项目验证优化方向。Jet 与 ProtoTracer 同为"在 ESP32 上纯软件渲染 3D 模型"的项目，且 Jet 明确以"低性能嵌入式、有限内存、可预测行为"为第一设计目标，是最贴切的参照系。

### 1.2 方法与范围

- **代码走读**（第一手）：`src/main.cpp`（1432 行全文）、`src/Render/`（Scene/Camera/Object3D/PixelGroup/QuadTree/Triangle2D）、`src/Math/Mathematics.h`、`src/Controllers/TasESP32S3KitV1.h`、`src/Flash/PixelGroups/P3HUB75.h`、`platformio.ini`、`docs/` 全部 7 篇；
- **Jet 走读**（第一手）：`Renderer.cpp`（1693 行核心）、`Scene.cpp`、`Scene.hpp`、`Renderer.hpp`、`Camera.*`、`Object.hpp`、`Math.hpp`、`FastMath.hpp`、`TrigLUT.hpp`、`Material.hpp`、`Texture.hpp`、`Shader.hpp`、`JetConfig.example.hpp`、`Jet.hpp`、`Sample.cpp`、`ObjLoader.h`、构建文件（CMakeLists/idf_component.yml/library.json）、README；
- **并行子代理深挖**：Jet 其余模块（PostFX/Primitives/LensFlare/ParticleSystem/Picking/Sprite2D）与 ProtoTracer 其余子系统（JsonDrivenProtogenAnimation 62 KB、ESPMenu BLE、Sensors、Network、web/android 端）——用于交叉验证；本报告全部关键结论均经第一手源码走读核实。

### 1.3 关于"Jet"的一点澄清

CubeCoders/Jet 仓库**当前内容已不是**早期的 Rust 游戏服务器守护进程，而是 **"tiny, dependency-free, fixed-function 3D rasteriser"（无依赖、固定功能 3D 软件光栅化库，C++17）**，面向 ESP32/STM32 等低性能 MCU，同时可跑桌面。仓库 `library.json` 描述为 "Portable software 3D renderer and scene pipeline for embedded and desktop targets"，许可 **AGPL-3.0**（与 ProtoTracer 相同）。本文后续的"Jet"均指这个渲染库。

---

## 2. Jet 项目速览

### 2.1 定位

- **库，不是应用**：Jet 不拥有窗口、显示驱动、主循环——宿主提供 RGB565 帧缓冲与 z-buffer，每帧调用一次 `scene->render()`，然后自行把缓冲推给显示器（"Jet stops at the framebuffer"）。
- **固定功能、编译期可裁剪**：每个特性都是 `JetConfig.hpp` 里的编译期开关，"只在 flash/RAM/CPU 上为你实际用到的功能付费"（见 §4.6 完整开关表）。
- **硬实时、固定预算**：热路径无分配、光栅化无虚函数、无隐藏全局；"同一场景在任何平台渲染结果一致"（确定性）。
- **目标美学**：晚期 90 年代主机/街机风格（flat/Gouraud/仿射纹理、RGB565），不是 PBR。

### 2.2 性能量级（README 官方数据）

- ESP32-S3：约 **40,000 三角形/秒**（flat 着色）；
- 480×320@60fps：可承载约 **650 个屏幕内三角形**（剔除后）；@30fps 约 **1300 个**；
- 官方演示：Wipeout 风格竞速游戏在 ESP32-S3 上 **60 FPS**（交错场缓冲模式，纯软件）。

> 对照：ProtoTracer 主场景为 128×32 面板共 **2048 像素**、面网格 **339 顶点/326 三角形/85 个 morph 目标**——三角形预算与 Jet 的 650 屏幕内三角形同量级，但像素数少两个数量级（2048 vs 153,600）。ProtoTracer 的瓶颈不是填充率而是**每像素开销（射线-三角形测试链）与每帧结构重建**。

### 2.3 构建与可移植性

- **ESP-IDF 组件**：`idf_component.yml`（idf ≥ 4.4）+ `CMakeLists.txt`（强制 C++17）；
- **Arduino 库**：`library.json`（frameworks: arduino, espidf）；
- **桌面**：CMake 子目录；`ESP_PLATFORM` 检测保证桌面构建不引入 `esp_attr.h`/FreeRTOS；
- **每个前端自带一份 `JetConfig.hpp`**（放在 include 路径上即可，无需改库源码）。

---

## 3. ProtoTracer 现状深度分析

### 3.1 总体架构与启动流程

- **启动阶段机**（`main.cpp`）：`Downloading → WifiTeardown → BleInit → Running`。
  1. setup()：ProtoGC 双堆初始化 → 显示器 I2C 探测（GLASS2/OLED/LCD，运行时 `new` 探测）→ 手势总线选择（GLASS2 共享 Wire vs 独立 Wire1）→ HUB75 DMA 驱动初始化（先于 WiFi/BLE 预留内部 SRAM）→ NetWizard 配网 → Gitee/GitHub **双源测速探针**（结果缓存 `/source_cache`，24 h 失效）→ 固件更新检查（`FirmwareUpdater`，按 manifest 比较版本）→ 资产下载（快路径：本地资产齐全则**后台下载**；慢路径：同步下载后继续）。
  2. loop()：后台下载任务（core 0，idle 优先级，120 s 超时）完成后 `DoWifiTeardownAndBleInit()`——**WiFi 关断、ProtoGC 全量回收、锁 malloc 至 PSRAM、再初始化 BLE**（WiFi 与 BLE 共存不佳，此顺序是刻意设计）；随后创建双核动画管线（见下）。
- **双核动画管线**（`ANIM_RENDER_PIPELINE`，默认开）：
  - `AnimationTask`（core 0，栈 8192 B，优先级 1）：信号量握手——主循环（core 1）先给 `gRenderDoneSemaphore`，任务执行 `animation.UpdateTime(ratio)` ＋ `PublishSceneVertices()`（把动画顶点快照 memcpy 到双缓冲渲染顶点），再给 `gAnimDoneSemaphore`；
  - core 1 等待期间执行 `animation.MenuUpdate()`（I2C 小屏、手势、BLE、NeoPixel），随后 `controller.Render(scene)` 光栅化并 `controller.Display()` 输出 HUB75；
  - 对象层 `EnableDoubleBuffer()`/`PublishVertices()` 保证动画核与渲染核不竞争同一顶点缓冲（`Object3D.h`）。
  - 另有 `CAMERA_RASTER_WORKER` 编译期选项：把后半像素区间派给第二个光栅任务（core 0，需 ≥512 像素才派发），默认关闭。

### 3.2 渲染管线（核心）

**范式：逐像素光线投射（ray-casting），不是光栅化。** 每帧流程（`Camera::Rasterize()`，`Camera.h`）：

1. **光线准备**：对 2048 个面板像素坐标做缩放（`scale`）后用 ESP-DSP 批量旋转（`dsps_mulc_f32`/`dsps_add_f32`，4 个内部 SRAM 浮点暂存数组 `tmpX/tmpY/rotX/rotY`，各 8 KB），结果写入 `cachedRays`（PSRAM）；
2. **投影**：对场景内每个物体的每个三角形，用 `Triangle2D(invView, camPos, ...)` 构造投影三角形（顶点变换到相机系、计算**平均深度** `averageDepth`、预计算重心分母 `1/det`）并插入 QuadTree；
3. **结构构建**：`QuadTree tree(bounds)` ＋ `tree.Rebuild()` —— 已在 arena 模式下复用 `Camera` 预分配的池（`mArenaTriangles[350]`、`mArenaNodes[128]`、`mArenaNodeRefs[8192]`，PSRAM），**每帧零堆分配**；
4. **着色**：对每个像素——`tree.Intersect(ray)` 下钻到叶子 → `CheckRasterPixel()` 对叶子内三角形逐一做重心测试（`Triangle2D::DidIntersect`，fmaf 版本，含 `TRIANGLE2D_USE_DSP` 可选 AE32 点积路径）→ 用**三角形平均深度**（非逐像素深度插值）做 z 遮挡 → 命中后 `Material::GetRGB(intersect, normal, uv)`（**纯虚函数，逐像素虚调用**）写 `pixelColors`。

**已应用的优化**（本次走读确认）：arena 池化 QuadTree、ESP-DSP 批量旋转、`BuildRotation2D` 四元数→2×2 矩阵、`Object3D::UpdateTransform()` 每帧构建 3×3 矩阵后逐顶点直乘（而非逐顶点四元数运算）、fmaf 重心测试、`averageDepth` 提前裁剪、`GetCameraMin/Max/CenterCoordinate` O(1) 化、可选双核光栅 worker。

**结构性弱点**（与 Jet 对比后尤为明显）：
- 逐像素 **QuadTree 遍历＋叶子内三角形测试**：像素数×叶子三角形数，是"像素驱动"而不是"三角形驱动"——对 326 三角形/2048 像素的小场景，直接光栅化（按三角形包围盒扫像素）理论工作量更小且无遍历开销；
- `averageDepth` 作为唯一深度依据，**大三角形/近相机时排序错误**（Jet 用同样的 `FAST_Z` 但配合 painter 桶排序兜底，见 §4.2）；
- `Material::GetRGB` 逐像素虚调用 + 昂贵材质（SimplexNoise/atan2/powf 等）无快速路径；
- **无背面剔除、无近平/远平面裁剪**（`Camera.h` 投影不做 near/far 剔除；Jet 在 `emitTri` 做 screen-bounds+背面剔除，`drawTriangle` 做 near/far 剔除，`Scene` 做 AABB 球面剔除）。

### 3.3 内存架构

- **稀缺资源是内部 DRAM（约 512 KB）**：DMA 缓冲、热路径数据、任务栈都在内部；PSRAM 8 MB 用于大/冷数据。
- **ProtoGC 双堆管理**（`protogc::ProtoGC`，BaiTian6641 的库）：内部堆管非 DMA 应用 SRAM，PSRAM 管冷分配（managed segments/pools/arenas）；`HeapGuard::onWarning/onCritical` 挂钩分级回收（light/emergency）；loop() 每 5 s `ProtoGC::poll()`；
- `heap_caps_malloc_extmem_enable(64)`：≥64 B 的普通 malloc 可落 PSRAM（此前为 0，导致 STL/String/JSON 全部挤占内部 DRAM——已放宽）；
- 显式放置：`EXT_RAM_BSS_ATTR uint16_t gHudBuffer[64*32]`（PSRAM）、`Scene::objects` 用 `heap_caps_aligned_alloc(16, ..., SPIRAM|DMA)`（PSRAM 对齐 16 B，附带内部 RAM shadow 表加速遍历）、`cachedRays`→PSRAM、arena 池→PSRAM、`USE_PSRAM_FOR_FACE_JSON` 把 JSON 解析放 PSRAM；
- HUB75 DMA 驱动降级链：8-bit 色深 → 降色深重试 → 关闭双缓冲重试（`TasESP32S3KitV1::Initialize()`），每次失败记录堆状态；
- `gHudBuffer`/`gHudColors` 机制：主循环直接读 `pixelColors` 转 RGB565 推 M5 小屏，**零额外任务**。

### 3.4 动画引擎（JsonDrivenProtogenAnimation，62 KB 头文件）

- `class JsonDrivenProtogenAnimation : public Animation<2>`（**注意：已从旧文档的 `Animation<1>` 升为 2**，背景对象可进场景了）；场景容量 2 = 脸 + 背景。
- JSON 驱动：`example_animation.json`（或设备专属 `{device_id}_animation.json`）定义 expressions（每帧 `ApplyExpression()` 驱动 morph 目标帧、材质、场景特效）、`auto_link`（viseme ↔ morph 自动关联）、`boop_morphs`（boop 次数→表情映射）、`flipped_morphs`（反向动画 morph）、`hue_shift` 绑定（材质→HueShift 处理器）；
- **本次走读确认已做的优化**：`resolvedAnimParams`（JSON 加载期把 morph 名解析为 `(uint16_t morphId, float)`，避免逐帧字符串扫描）、`activeBoopExpressionIndex = -1`（int16_t 索引代替 String）、`FindExpressionIndex` 缓存、morph 名→ID 解析器单次化；`Initialize()` 顺序为 `LoadAnimationConfig → AutoLinkMorphs → LinkParameters`（`validate.ps1` 会校验）。
- 每帧路径：`UpdateTime(ratio) → Update(ratio)`；`Update()` 内部做表情应用（含 `ApplyExpression` 按需字符串查找）、EasyEase 插值、morph 混合、FFT 口型（viseme）、眨眼计时、boop 状态机；
- 材质体系：`MaterialAnimator<10>`（**当前代码已修复越界：`AddMaterial` 用 `currentMaterials < materialCount`，循环用 `<`**）＋ `CombineMaterial<10>` 图层混合；注册表 `materialRegistry`/`effectRegistry`（名→指针）；动态材质/特效由 JSON 注册并 `unique_ptr` 持有；
- 每帧仍存在的工作：部分字符串查找路径、`EasyEaseAnimator` 每帧遍历、逐像素昂贵材质求值（见 §3.2 弱点）。

### 3.5 网络 / OTA / 资产同步（子代理细节并入）

- 三件套：`RemoteFileSync`（HTTP + MD5 校验下载）、`FaceModelUpdater`（脸模型 JSON 同步）、`FirmwareUpdater`（远程 manifest `esp32s3.json` 版本比较 + OTA）；`AnimationDownloader` 处理动画 JSON；
- Gitee/GitHub 双源：启动测速探针（TCP connect 延迟，Gitee 默认优先，GitHub 快 >100 ms 才切换），结果缓存于 `/source_cache`；
- 启动快路径：本地资产齐全 → 固件检查 → **后台 FreeRTOS 下载任务**（core 0、idle 优先级、8192 B 栈、120 s 超时）并行同步 user_config/face/animation，期间首帧立即渲染；
- 下载完成后 WiFi 关断（`WiFi.disconnect(true)` + `WIFI_OFF` + `server.end()`）→ ProtoGC `collectFull` → BLE 初始化；
- NetWizard 负责配网（captive portal），ElegantOTA(ElegantOTAPro 若存在) 提供 `/update` 网页 OTA；长按 OTA_BTN 1.2 s 进 AP OTA 模式（30 s 冷却）。

### 3.6 BLE 遥控（ESPMenu.h，子代理细节并入）

- 服务 UUID `73cf57c7-6797-46e8-8202-dc5e7f956b57`；JSON 指令：`config.get`（→manifest）、`control.set`（→`control.state`）、`ping`（→`pong`）；
- 载荷按 **160 B 分块**通知；hue 契约：manifest `visual.red/green/blue` 为用户基准色，`control.set hue_shift` 是相对基准色的绝对旋转（`(target − baseHue) mod 360`），web/android 两端与固件三方一致；
- 发送路径已是**非阻塞分块**（每主循环迭代一块，无 `delay`）；RX 缓冲有预留上限与溢出丢弃；manifest/响应 JSON 用 PSRAM 分配器；共享状态有临界区保护（详见 §3.9 核对表）。

### 3.7 显示输出（HUB75 + M5 预览）

- `TasESP32S3KitV1`：两块 64×32 P3 面板链成 128×32 虚拟屏（`ESP32-VirtualMatrixPanel-I2S-DMA`，`double_buff=true`，8-bit 色深）；`Display()` 把 `pixelColors`（2048 像素）经 `fillBufferRgb888` 写 DMA 后缓冲并 `flipDMABuffer()` 原子翻转——**DMA 扫描前缓冲与 CPU 写后缓冲并行**；
- 亮度：`setBrightness8` 仅在变化时调用（已缓存）；HUB75 色序补偿：开机 RGB 测试 → 用户配置 `hub75_color_order` 记录实际看到的顺序 → 引脚重映射（`ApplyHub75ColorOrder`）；
- M5 预览：`gHudColors` 指针导出 → ESPMenu 转 RGB565 推小屏（I2C），`ENABLE_M5_PIXEL_PREVIEW` 编译期开关（RELEASE/PROFILE 开）；
- `P3HUB75.h`：`const static Vector2D P3HUB75[2048]` 编译期坐标表（16 KB flash），实际是**严格 3 单位等距网格**（0,3,6,...）——完全可以用 `(x*3, y*3)` 解析计算替代（见 §6.F）。

### 3.8 构建配置

- PlatformIO 4 个环境：`esp32s3`（debug，`-O3 -g`）、`esp32s3-RELEASE`（release，开 `USE_TOKEN_AUTH`/`ENABLE_M5_PIXEL_PREVIEW`）、`esp32s3-PROFILE`（`-O2 -Wall` + `PRINTINFO` 遥测）、`esp32p4`；
- pioarduino 平台（Arduino-ESP32 3.x / IDF 5.x），**C++17 必需**（ProtoGC 与 `std::make_unique` 等）；NetWizard 锁 `^1.2.0`；
- 功能开关以 `-D` 宏散落各处：`TASESP32S3/TASESP32P4`、`NEW_GESTURE`、`USE_TOKEN_AUTH`、`ENABLE_M5_PIXEL_PREVIEW`、`LANG_CN`、`OTA_BTN`、`USE_PSRAM_FOR_FACE_JSON`、`HUB75_*`、`BOOP_DEBUG_LOG`、`VERBOSE_STARTUP`、`PRINTINFO`、`CAMERA_RASTER_WORKER`、`ANIM_RENDER_PIPELINE` 等约 30 个；
- 额外脚本：`select_elegantota.py`（付费库回退）、`merge-bin.py`（合并固件）、`set_mklittlefs.py`；`generate_md5s.sh` 生成资产 MD5；`validate.ps1` 校验 viseme 引用与初始化顺序。

### 3.9 与既有文档（2026-05-31）的差异核对

| 文档记录的问题 | 当前代码状态（本次走读） |
|---|---|
| `MaterialAnimator<10>` 越界（`<=`） | ✅ 已修复（`< materialCount` / `< currentMaterials`） |
| 邻居表 4×2048 个 `unsigned int`（32 KB） | ✅ 已改 `uint16_t`（16 KB），且 HUB75 规则网格可完全去掉 |
| `Animation<1>` 放不下背景对象 | ✅ 已升 `Animation<2>` |
| boop 表达式 String 拷贝 | ✅ 已改 `int16_t activeBoopExpressionIndex` |
| morph 名逐帧字符串扫描 | ✅ 已加 `resolvedAnimParams` 预解析 |
| `cachedRays` 在内部 DRAM | ✅ 已迁 PSRAM |
| `Display()` 逐像素 `GetColor()` 虚调用 | ✅ 已改 `GetColors()` 直接指针 |
| `heap_caps_malloc_extmem_enable(0)` | ✅ 已放宽为 `extmem_enable(64)` |
| QuadTree 每帧分配 | ✅ 已 arena 化（Camera 预分配池） |
| 每帧 `esp_timer_create/delete`（麦克风） | ✅ 已修复（`MicrophoneFourier_MAX9814.h:262` 创建一次，`esp_timer_start_periodic` 常驻） |
| BLE 通知 `delay(8)` 阻塞 | ✅ 已重写为非阻塞（`NotifyBleJsonPayloadChunk()` 每次主循环只发一块，`ESPMenu.h:356-396`） |
| manifest 大 JSON 在内部 DRAM | ✅ 已用 PSRAM 分配器（`BasicJsonDocument<protogc::ProtoJsonPsramAllocator>`，`ESPMenu.h:457`） |
| BLE 回调内 `String` 无界累积 | ✅ 已设上限与预留（`kBleRxJsonBufferBytes` + `reserve`，溢出即丢弃，`ESPMenu.h:785-790,1285`），共享标志已加临界区（`gBleFlagMux`/`gCommandQueueMux`） |
| 屏幕特效（blur）通道错位等正确性 bug | ✅ 已修复（`VerticalBlur.h:55-58` 通道正确、`blurRange` 计算一次、按实际采样数除；`RadialBlur.h:48` 已改用 `indexD`） |
| `FreeMem()` 误导性遥测 | ⚠️ 仍存在（但已新增 `GetFreeInternalDRAM()` 等真实遥测并用之） |

> 含义：**既有的"修复期"（Milestone A/B）大体已完成，当前处于"提速期"**——正是引入 Jet 经验的最佳时机。

---

## 4. Jet 深度分析

### 4.1 设计哲学

1. **固定功能**：`ShadingMode` enum（FLAT/GOURAUD/PHONG/WIREFRAME/UNLIT/WATER_REFLECT/ADDITIVE）+ 少量自定义 shader 入口；不做 PBR/GI；
2. **编译期特性矩阵**：`JetConfig.hpp` 约 40 个开关（§4.6 全表），"不为未用功能付费"；
3. **热路径零分配、零虚函数**：每帧 `transformedVertices.resize(vertCount)` 复用静态 `std::vector`；`renderQueue`/`renderOrder` 容量预置；排序只搬 4 B 索引不搬结构体；光栅化内部无虚调用（材质采样是非虚方法，着色走 enum switch）；
4. **确定性**：固定预算、无隐藏全局、无平台相关行为（`PERF_CRITICAL = IRAM_ATTR` 仅在 ESP32 上生效，桌面为空）；
5. **库边界**：止于帧缓冲，宿主自备显示驱动——渲染器可无硬件验证（桌面跑同一份代码）。

### 4.2 渲染管线（`Renderer.cpp`，1693 行）

每帧（`Scene::render()` → `prepareFrame()` + `rasterizeBand()`）：

1. **变换**：物体矩阵（定点，int64 中间量）＋相机矩阵；`renderObject` 逐顶点变换到相机空间，随后透视投影到屏幕空间（整数坐标）；
2. **剔除链**：`cullObject()` **AABB 包围球视锥剔除**（每帧缓存视锥侧平面法长）→ 物体开关/LOD 选择（`lodMeshes`，距离驱动）→ per-object fade（`fadeNear/fadeFar`、`appearNear/appearFar`，距离淡入淡出）→ `emitTri` 屏幕边界裁剪＋**背面剔除**（`SKIP_ZERO_AREA_TRIANGLES`：零/负面积即背面）→ near/far 平面剔除；
3. **排序**：**painter 桶排序**——`renderQueue` 按 `avgZ − zBias×256` 计数排序进 K=64 桶（O(N)，稳定性保留），**只排 4 B 索引**（`renderOrder`）而非结构体；三带：`noWriteZBuffer`（先画，如天空）→ 主带（远→近）→ `ignoreZBuffer`（最后、无条件最上）；
4. **光栅化**（`drawTriangle`，`PERF_CRITICAL` 即 IRAM_ATTR）：
   - 包围盒（`& ~1` 对齐半宽像素）→ `FAST_Z`：三角形平均 z（**与 ProtoTracer `averageDepth` 同思路**；`LAZY_Z` 可选取最远顶点）；`Z_BUFFERING` 可编译期关闭（默认关，省 2 B/像素）；
   - **增量边函数**：预计算 `dw/dx`、`dw/dy`（int32），行首用 int64 算 `w0_row/w1_row/w2_row`，逐像素仅 **int32 加法**；
   - **扫描线范围求解**（`JET_EDGE_RANGE` 宏）：不逐像素测边，直接解出本行 `[iStart, iEnd]` 覆盖区间（ceil/floor 除法，且用 32 位硬件除法规避 70 周期软除法 `__divdi3`）；
   - **快速跨度填充**（`JET_FAST_SIMPLE_SPANS`，条件全满足时自动启用）：`fillRGB565Span` 成对 32 位写，ESP32-S3 上进一步用 **PULP SIMD `ee.vst.128.xp` 128 位矢量存储**（quads 循环每次退 64 B）；
   - 一般路径（`!JET_FAST_SIMPLE_SPANS`）：逐像素边函数累加器＋`(ew0|ew1|ew2) < 0` 快速内侧测试＋z-buffer（可选）＋Gouraud **Q16 增量亮度**（`brightness_q16 += brightness_dx_step_q16`，每像素一次 int32 加法替代除法和 3 次乘法，~30× 便宜）＋Phong 逐像素（`PERSPECTIVE_CORRECT_TEXTURES` 时才插值法线）；
   - 着色/混合：`blendRGB565` 用 `(256−a)>>8` 一次移位替代除以 255；`addBlendRGB565` 饱和加；**screen-door alpha**（4×4 Bayer 阈值矩阵，alpha>240 全画；HALF_WIDTH 下退化为每行两个布尔）；`DEPTH_ALPHA_BLEND` 距离雾在 FAST_Z 下提升为三角形级 hoist；
   - 专用路径：WATER_REFLECT（屏幕空间反射 + `lookupSinI` 涟漪）、ADDITIVE（霓虹/光晕）、wireframe 模式（每行最多 2 次存储）、纹理 LOD（距离衰减到纯色快速路径）、`DEBUG_OVERDRAW`（橙色叠加显示过绘）、`MAX_PICK_QUERIES` 拾取（0 时整段编译消失）；
5. **并行带**：`rasterizeBand(yMin,yMax)` 用线程局部光栅器副本，支持多线程无重叠 y 带并行（桌面/多核），`skipWaterReflect/waterReflectOnly` 两遍法解决 SSR 跨带读依赖。

### 4.3 数学体系（混合定点/浮点）

- **定点 Q10**（`FIXED_POINT_SCALE = 1<<10 = 1024`）：顶点位置、变换、边函数、深度全为 int32 定点；`JET32_WORLD_SCALE = 8` 对世界坐标预放大获得亚像素精度（消除 PS1 式边缘抖动）；
- **int64 中间量**：矩阵乘、边函数行首值、`denom64`（近相机投影坐标可超 int32 范围）——**只在设置阶段用 64 位，逐像素热循环保持 32 位**；
- **混合 float**：`FLOAT_CAMERA_ANGLES` 下相机角为 float 弧度；`Vec3f` 仅用于构建期/物理；`renderObject` 预除出 float 矩阵副本（`fObjM00..`），逐顶点矩阵-向量用 float（**Xtensa LX7 FPU 单指令，比定点乘加 + 移位快**）；`invDenom64f = 1.0f/denom64` 用 float 倒数替代两处 int64 除法（~70 cy → ~5 cy）；
- **三角 LUT**：`sin_table[360]`/`tan_table[360]`（int32 定点）＋ float 版本 `float_sin_table[3600]`（`FLOAT_SIN_CACHE_SCALE=10` → 0.1° 分辨率）；`lookupSinI(angle)` 为取模查表；
- **sqrt/rsqrt**：直接 `std::sqrt`（FPU 单指令，注释明确说比牛顿迭代近似"actually correct"）；
- 整数除法纪律：扫描线范围求解里用技巧保证**除法前操作数必在 int32 范围**，从而走 Xtensa 硬件 32 位除法而非 64 位软除法。

### 4.4 内存模型

- 宿主提供：`uint16_t framebuffer[W*H]`（RGB565）＋ 可选 `uint16_t zBuffer[ZBUFFER_STRIDE(W)*H]`；`Z_BUFFERING=0` 时 zBuffer 可为 nullptr；
- **`HALF_WIDTH_BUFFERS=1`**（默认）：帧缓冲每个 `uint16_t` 覆盖**两个相邻输出列**（水平半分辨率渲染，扫描输出时每像素翻倍）——帧缓冲 RAM 与填充带宽减半；
- **`FIELD_BUFFERS=1`**（默认）：帧缓冲拆成两个半高场（奇偶行），逐帧交替渲染一个场（**交错模式**），显示驱动 DMA 同时读**另一个**场——消除 CPU/DMA 带宽竞争，渲染与输出真正并行；官方 60 FPS 演示即此模式；
- `RenderVertex` 瘦身：仅携带配置实际消费的字段（无纹理/无光照时 12 B 而非 36 B，队列拷贝流量 ×3）；
- `RenderTri` 队列 + `renderOrder` 索引：**排序不复制结构体**；
- `transformedVertices` 静态 vector 每帧 `resize` 复用；`Object::Vertex` 作者型数据（pos/uv/normal/color/lambertBrightness）与渲染型数据分离；
- 无动态分配热路径：注释明示 "no allocations on the hot path, no virtual dispatch in the rasteriser, no hidden globals"。

### 4.5 其余模块

- **Primitives**（`Primitives.cpp`，26 KB）：`createCube/DebugCube/Grid/Plane/Pyramid/Sphere/Capsule/Cylinder/Quad/Billboard` 程序化几何生成器——所有生成在**构建期/启动期**完成，运行时零开销；`computeFlatNormals` 把面法线印到三个顶点上（FLAT 着色免逐三角形平均）。
- **PostFX**（`PostFX.hpp/.cpp`）：6 种后效（FXAA/bloom/CRT/motion blur/chromatic/pixelate），每个 `POSTFX_*` 为 0 时**编译为 no-op**；工作缓冲（bloomBuffer/bloomTemp/previousFrame/fxaaTopRow）构造期一次分配、每帧复用；"免缓冲"类（CRT/cellshading）与"需全屏缓冲"类（其余）在配置里明确分开——小内存目标只启用前者。
- **Sprite2D**（`Sprite2D.hpp`）：屏幕空间 2D 叠加层（纯色矩形或纹理 blit），`zOrder` 排序，色键透明，alpha=255 直拷快速路径；在 `HALF_WIDTH_BUFFERS` 构建里由显示层在全分辨率扫描输出时合成。
- **ParticleSystem**（`ParticleSystem.hpp`，374 行）：**固定池**（`Particle pool[200]`，无每粒子堆分配）；`render()` 把粒子直接喂给 `Rasterizer::drawTriangle()`（绕过场景图，合成在场景之上）；spark/splash 两个发射器带距离 LOD 剔除。
- **LensFlare**（`LensFlare.hpp`，315 行）：方向光投影到屏幕点 → 可选拾取遮挡测试（`MAX_PICK_QUERIES=0` 时视为无遮挡）→ 沿"太阳→屏幕中心"轴重排固定 Sprite2D 链并调 alpha；元素自带 BLEND_ADD 混合与整数放大。
- **Picking**（`Picking.hpp`）：`MAX_PICK_QUERIES` 编译期上限；为 0 时**整个特性（含光栅器内的逐行测试、RenderTri 标签、Scene/Rasterizer 拾取状态）从二进制消失**——"零运行时代价"的教科书示例。
- **ObjLoader**（`ObjLoader.h`，334 行）：最小 Wavefront `.obj` 加载器（顶点/法线/UV/面/材质名），另含 16-bit BMP 纹理加载。
- **Light**（`Light.hpp`）：`DirectionalLight`（方位角/仰角 → 定点点积光方向）、`AmbientLight`（**每通道环境色**——阴影带冷/暖色调而非中性灰）、`PointLight`（距离衰减，场景按距离取最近几个参与计算）。
- **Example/Sample**：`Sample.cpp` 是 mbed 风格最小示例（framebuffer + cube/plane/sphere + 每帧 rotate/render）；README 的 minimal example 展示了完整宿主契约（`scene.setBackcolor/setClearBuffer/setCamera/setDirectionalLight/addObject` + 每帧 `render()`）。

> 注：本节全部模块均经第一手源码走读核实（详见附录 A 文件清单）。

### 4.6 编译期配置全表（`JetConfig.example.hpp`，含默认值）

| 开关 | 默认 | 作用 |
|---|---|---|
| `JET32_WORLD_SCALE` | 8 | 世界坐标预放大（亚像素精度） |
| `RENDER_TILE_BUFFER` | 0 | 每帧脏瓦片位图（瓦片型 SPI 屏跳过未变瓦片） |
| `FAST_Z` | 1 | 三角形平均 z 替代逐像素插值 |
| `LAZY_Z` | 0 | 取最远顶点 z（配合 Z_BRIGHTNESS 防大三角形误暗） |
| `SCREEN_DOOR_ALPHA` | 1 | 4×4 Bayer 抖动透明 |
| `NOISE_ALPHA` | 0 | 噪声抖动透明（与上互斥） |
| `SKIP_ZERO_AREA_TRIANGLES` | 1 | 零/负面积三角形剔除（含背面） |
| `Z_BUFFERING` | 0 | 逐像素深度缓冲（W×H×2 B） |
| `SORT_TRIANGLES` | 1 | 渲染队列按深度排序（painter） |
| `SORT_SCENE_OBJECTS` | 0 | 物体按距离排序（配 Z_BUFFERING 早退） |
| `DEPTH_ALPHA_BLEND` | 1 | 距离雾 |
| `TEXTURE_MAPPING` | 0 | 仿射纹理 |
| `PERSPECTIVE_CORRECT_TEXTURES` | 0 | 逐像素 W 除法的透视校正 |
| `BILINEAR_FILTER` | 0 | 双线性过滤 |
| `LIGHTING` | 0 | 环境+方向光（顶点成本约 ×2） |
| `Z_BRIGHTNESS` | 0 | 距离变暗 |
| `FLOAT_CAMERA_ANGLES` | 1 | float 相机角 + 定点 LUT 索引 |
| `FLOAT_SIN_CACHE_SCALE` | 10 | sin LUT 0.1° 分辨率 |
| `HALF_WIDTH_BUFFERS` | 1 | 半宽帧缓冲（RAM/带宽减半） |
| `FIELD_BUFFERS` | 1 | 交错场缓冲（渲染/输出并行） |
| `SSR_FIELD_REFLECT` | 1 | 水面反射读上一场 |
| `POSTFX_CRT/CELLSHADING` | 0 | 免缓冲后效 |
| `POSTFX_ANTIALIASING/BLOOM/MOTION_BLUR/CHROMATIC/PIXELATE` | 0 | 需额外全屏缓冲的后效 |
| `DEBUG_OVERDRAW` | 0 | 过绘可视化 |
| `CHECKERBOARD_MODE`(+`_RECONSTRUCTION`) | 0 | 棋盘格半分辨率渲染 |
| `MAX_PICK_QUERIES` | 0 | 屏幕拾取（0 = 整段代码不编译） |

> **要点**：默认配置（fast-simple-spans 生效）≈ 无 z-buffer、无光照、无纹理、无后效的"最瘦"渲染器；开启功能只在你真正需要时。这与 ProtoTracer"所有功能常驻编译"形成鲜明对照（见 §6.F）。

### 4.7 性能成本模型（源码注释中的关键数字）

- 逐像素 Gouraud 增量：每像素 1 次 int32 加 ≈ 原来（除 1 次 + 3 乘）×30；
- `invDenom64f` float 倒数：~70 cy → ~5 cy（每三角形）；
- 32 位硬件除法 vs `__divdi3`：~5x 差距，扫描线求解器专门保证走硬件除法；
- FLAT 光照：直接取 v1 法线免 sqrt 归一化（"costing nearly 2x the no-lighting frame time"的修复）；
- 无纹理 FLAT/GOURAUD 时每三角形把调制 hoist 到三角形级（`flatColorPrecomputed`）。

---

## 5. 对比分析（总表）

| 维度 | ProtoTracer（现状） | Jet | 差距含义 |
|---|---|---|---|
| 渲染范式 | 像素驱动 ray-casting + QuadTree | 三角形驱动扫描线光栅化 | 小场景下直接光栅化工作量更小、无遍历 |
| 数学 | 全 float + ESP-DSP 批处理 | Q10 定点 + int64 设置期 + float 热循环 | 混合策略：热循环整数增量更省；float 有 FPU 兜底 |
| 深度 | 平均深度 z 遮挡（无排序） | FAST_Z + painter 桶排序（默认）| 平均深度必须配排序才正确 |
| 每像素开销 | 虚函数 GetRGB + 材质重计算 | enum switch + 三角形级 hoist + 增量式 | 可量化优化空间大 |
| 剔除 | 仅 enabled 标志 | AABB 球面视锥 + 背面 + near/far + LOD | 可移植整条剔除链 |
| 热路径分配 | 无（arena/缓存复用） | 无（静态 vector 容量预置） | 一致；注意 Jet 的 vector 在 ESP32 上预置后也不再增长 |
| 每帧结构 | QuadTree 重建（arena） | 桶排序 + 索引重排 | 后者对 326 三角形更便宜 |
| 输出 | HUB75 I2S DMA 双缓冲（无帧缓冲概念） | RGB565 帧缓冲 + half-width/field 模式 | 概念不同，但 field 模式思想可迁移（§6.G） |
| 功能裁剪 | ~30 个散落宏 | ~40 个系统化开关（含"整段代码消失"） | 可借鉴 JetConfig 模式 |
| 可移植测试 | 无单元测试、纯硬件验证 | 桌面 CMake 同码运行 | 建议抽渲染核心做桌面 harness |
| 依赖 | 大量（M5Unified/NetWizard/ArduinoJson/…） | 零依赖 | Jet 的"零依赖"在嵌入式中是巨大可维护性优势 |
| 功能广度 | 动画引擎/BLE/OTA/网络/传感器（应用） | 纯渲染库（无应用层） | 互补：Jet 深度在渲染，ProtoTracer 广度在系统 |
| 许可 | AGPL-3.0 | AGPL-3.0 | 可合法参考/借鉴实现（需遵守 AGPL 传播条款） |

---

## 6. 可借鉴的优化方向（重点章节）

> 排序按"性价比 × 与既有计划的衔接度"。每项标注：**对应 ProtoTracer 位置 / 借鉴来源（Jet 文件） / 与 `optimization-plan.md` 的关系 / 预期收益**。

### A. 直接三角形光栅化（最高优先级，确认既有 Option C）

- **现状**：`Camera::Rasterize()` 逐像素走 QuadTree（§3.2）；既有计划 §7.3 已提出 Option C，但无参考实现。
- **Jet 参照**：`Renderer.cpp` 全套（drawTriangle/增量边函数/扫描线范围求解/FAST_Z）。
- **建议**：按 `optimization-plan.md` §7.3 落地"固定三角形光栅化"，但采用 Jet 的**增量边函数 + 扫描线范围求解**（而非朴素逐像素重心测试）：
  1. 每帧：物体矩阵变换 → 投影 → 包围盒钳制到 128×32 → 背面剔除（面积符号）→ near/far 剔除；
  2. 逐三角形：预计算 `dw/dx、dw/dy`（int32）与行首 `w0/w1/w2`（int64），按行求解 `[iStart,iEnd]`（ceil/floor，注意用 32 位硬件除法技巧）；
  3. 2048 像素 z-buffer（`uint16_t`，4 KB）或沿用 `averageDepth` + 新增 painter 桶排序（见 C）；
  4. 保留 `pixelColors` 直写语义（`pixelGroup->GetColors()`），Display() 无需改动。
- **预期**：消除 QuadTree 遍历与每像素叶子测试；帧时主要取决于被覆盖像素 × 三角形，326 三角形/2048 像素场景可望进入个位数毫秒级（Jet 同芯片 40 k tris/s 量级为参照）。
- **注意**：保留旧路径于编译开关下做 A/B（既有计划 §7.3 同样要求）；先核对 P3HUB75 坐标方向/镜像语义，避免视觉回归。

### B. 扫描线范围求解与 32 位除法纪律（随 A 落地）

- Jet 的 `JET_EDGE_RANGE` 宏是"不逐像素测边"的关键：行内 `[iStart,iEnd]` 由一次 ceil/floor 算出；且用"先证明商在 int32 范围，再走硬件 32 位除法"避免 64 位软除法（Xtensa 上 ~5× 差距）。
- ProtoTracer 目前 `DidIntersect` 对每个候选三角形做 fmaf 重心测试（无除法，但逐像素逐三角形）；直接光栅化后同样适用上述技巧。

### C. 深度策略：FAST_Z + painter 桶排序（纠正 averageDepth 的已知缺陷）

- **现状**：ProtoTracer 只用 `averageDepth` 做 z 遮挡（`CheckRasterPixel`），大三角形/近相机时会排序错误；没有排序兜底。
- **Jet 参照**：`FAST_Z`/`LAZY_Z` 默认组合——三角形平均 z 作深度键，但配合 **O(N) 桶排序**（K=64，`renderOrder` 只排 4 B 索引，三带：先画 noWriteZBuffer → 主带远→近 → ignoreZBuffer）。
- **建议**：直接光栅化（方案 A）落地时，z 策略二选一：
  1. 保留逐像素 z-buffer（4 KB，正确性最好，Jet 默认关但 ProtoTracer 场景小、负担得起）；
  2. 或 `averageDepth` + 桶排序（内存最省，效果等同 Jet 默认配置）。
- **额外**：Jet 的 `zBias`（decals 防共面闪烁）与 `DEPTH_ALPHA_BLEND` 距离雾（FAST_Z 下三角形级 hoist）可直接借鉴实现。

### D. 数学层：定点/浮点混合、LUT、除法纪律

- **TrigLUT**（`TrigLUT.hpp/.cpp`）：360 项 int32 sin 表（+0.1° float 表）——ProtoTracer 动画/相机每帧大量 `sinf/cosf`（如 `EasyEase`、`Mathematics::CosineInterpolation`、`GetRadialIndex`、材质 `atan2`/`powf`），可用查表 + 线性插值替代；收益在动画更新路径（非渲染热循环）立竿见影且零风险。
- **int64 设置期 + int32 热循环**：Jet 在"可能溢出的设置计算"用 int64，逐像素只做 int32 加；ProtoTracer 的 float 路径无溢出问题，但若未来引入定点边函数，遵循同一纪律。
- **float 倒数技巧**：`invDenom64f`（1 次 float 除法换 2 次 int64 除法）；ProtoTracer 的 `Triangle2D` 已在构造期预计算 `1/det`（好），可推广到投影除法。
- **不要盲目全定点化**：ESP32-S3 有 FPU，Jet 自己也在热循环用 float（矩阵-向量）；ProtoTracer 现有 float + ESP-DSP 批处理路线与 Jet 的定点路线**殊途同归**，建议只对"每帧重复的三角函数/除法"做定点/查表化，不做全局重写。

### E. 无分配热路径的收尾与内存瘦身

- **已达标项**（确认）：arena QuadTree、cachedRays→PSRAM、双缓冲顶点快照、Display 直指针。
- **Jet 参照**：`RenderVertex` 瘦身（12 B vs 36 B）、`renderOrder` 索引排序、静态 vector 预置容量。
- **建议**：
  1. `Triangle2D` 当前 16 字节 float 字段 × 350 的 arena 池（PSRAM）在直接光栅化后可用**更瘦的投影三角形结构**（int32 定点或打包），减少 arena 占用与带宽；
  2. 若保留 QuadTree 过渡期，`Scene::objectsShadow`（内部 RAM 镜像表）可在直接光栅化后删除；
  3. `P3HUB75` 16 KB 表 → 生成期内联计算（`x*3, y*3` 网格）或紧凑 `baseX/baseY` int16 数组（与既有计划 §3.4 一致）；
  4. 邻居表（`up/down/left/right`，已 uint16_t）在直接网格光栅化后**整组删除**（特效改用 `y*64+x` 直索引，既有计划 §9.1）。

### F. 编译期特性矩阵（JetConfig 模式）

- **现状**：ProtoTracer 的宏散落且粒度粗；`src/Flash/ImageSequences/*.h` 每个 3–4 MB（BadApple/Wave/Rick 等 13+ 个序列）**全部编译进固件**——flash 占用巨大，且与 Jet"只为你用的功能付费"哲学相反。
- **建议**：
  1. 建立 `BuildConfig.h`（或沿用 platformio.ini 宏）特性矩阵文档，明确每个特性的 flash/RAM 成本（对照 JetConfig.example.hpp 的注释风格）；
  2. **ImageSequences 按需编译**：按 env 宏（如 `SEQUENCES_MINIMAL`）用 `#if` 包住各序列，或改用 LittleFS 存放 + 流式播放（配合现有 RemoteFileSync 资产管道）；
  3. 后效/特效分级：`POSTFX_*` 式开关（既有计划 §9.3 效果分级）与 Jet 的"免缓冲 vs 需缓冲后效"分类一致；
  4. `CAMERA_RASTER_WORKER`、`TRIANGLE2D_USE_DSP`、`ANIM_RENDER_PIPELINE` 等已有开关应纳入矩阵并给默认值建议（RELEASE 默认关闭调试类开关：`BOOP_DEBUG_LOG`、`VERBOSE_STARTUP`）。

### G. 显示输出：批量填充与"场"的思想

- **HUB75 侧**：`fillBufferRgb888` 已是批量写；Jet 的 `fillRGB565Span` 成对 32 位写 + PULP `ee.vst.128.xp` 128 位矢量存储表明——**对规则网格的批量填充可用 SIMD 矢量存储再提速**（ESP32-S3 支持 PULP 扩展；ProtoTracer 面板 128×32，每帧 4096 字节×2 缓冲，仍值得）；需要 HUB75 库暴露后缓冲直写指针。
- **场/半宽思想**：Jet 的 `HALF_WIDTH_BUFFERS`/`FIELD_BUFFERS` 针对"CPU 渲染 vs DMA 扫描争带宽"。HUB75 本身是逐行扫描（1/32 扫）+ I2S DMA 双缓冲，**天然就是"场"结构**：可评估"隔行渲染"——奇数帧只渲染/更新偶数行像素，利用人眼残影在 2048 像素面板上换取接近 2× 的渲染预算（视觉质量需实测，作为可选项）。
- **M5 预览**：`ENABLE_M5_PIXEL_PREVIEW` 已存在；建议加"节流到 2–5 Hz + 只画状态图标"（既有计划 §6.1），Jet 的 `RENDER_TILE_BUFFER` 脏块思想可迁移到小屏预览（只推变化块）。

### H. 桌面可移植性与测试基建（长期高价值）

- **Jet 参照**：同一份代码三平台构建（桌面 CMake / Arduino / ESP-IDF），`ESP_PLATFORM` 隔离硬件头；官方文档即 Doxygen 生成并发布 GitHub Pages。
- **建议**：
  1. 把渲染核心（Math/Render/部分 Materials）抽成**无 Arduino 依赖的模块**（`#if __has_include(<Arduino.h>)` 隔离），建一个桌面 CMake target + 最小 SDL/fbdev 输出；
  2. 在桌面跑：单元测试（重心测试、投影、z 排序、特效数学）、**逐帧视觉回归**（golden frame 对比）、性能基准（同一场景在 x86 与 ESP32 上的相对预算）；
  3. 这直接补上 AGENTS.md 承认的"无单元测试"短板，且不增加固件 flash 成本。
- 注意：ProtoTracer 的动画/网络/BLE 与 Arduino 深度耦合，**只抽渲染核心**即可，不必全工程可移植。

### I. 着色与材质路径去虚化

- **现状**：`Material::GetRGB` 纯虚函数逐像素调用（§3.2）；昂贵材质（SimplexNoise、SpiralMaterial 的 atan2/powf、GradientMaterial 的 fmodf/sqrtf）无快速路径。
- **Jet 参照**：非虚 `Material::getColor` + `ShadingMode` enum switch；FLAT 三角形级 hoist（`flatColorPrecomputed`）；Gouraud Q16 增量；无纹理三角形走快速跨度路径（`useFastSimpleSpan` 每三角形判定）。
- **建议**（与既有计划 §7.5 一致，补充 Jet 式实现细节）：
  1. 热路径按材质类型分派（`Material::Method` enum 已存在）——switch 而非虚函数；
  2. **逐帧预计算材质**：把 2048 像素的材质输出预渲染成 64×32 调色板/纹理（材料变化慢时每帧一次，替换逐像素求值）——Jet 的 `Texture` 调色板动画（`advancePalette`）是同类思路；
  3. 单材质全不透明时跳过 `CombineMaterial` 混合（既有计划 §8.5）；
  4. 简单表情默认用 `SimpleMaterial` 快速路径，昂贵材质显式 opt-in（既有计划 §9.3 分级）。

### J. 剔除链与调试工具

- **剔除**：Jet 的"包围球视锥 → 背面 → near/far → LOD"四级剔除链可直接映射：ProtoTracer 场景固定（脸 + 可选背景），至少应加**背面剔除**（326 三角形省一半）与 **near/far 平面剔除**（`averageDepth` 已有 z，只差比较）；视锥剔除对 64×32 小 FOV 场景收益有限，可后置。
- **调试工具**（Jet 免费附赠的思路）：
  - `DEBUG_OVERDRAW`：过绘可视化——ProtoTracer 的 ray-casting 天然过绘高，此工具能直接指导优化优先级；
  - `wireframeMode`：单行内最多 2 次存储的线框调试模式；
  - 每帧计数器 `lastFrameDrawnObjects/DrawnTriangles/RasterizedTriangles`：**渲染统计入帧遥测**（ProtoTracer 的 `PRINTINFO` 已打印 anim/render 帧时，可加三角形/剔除统计）。

### K. 其余可迁移小技巧（低风险）

- `blendRGB565` 的 `(256−a)>>8` 代替 `÷255`；
- screen-door alpha（4×4 Bayer）替代全屏半透明混合——HUB75 低色深下视觉更稳（既有计划的"boop 闪光/变色反馈"可参考）；
- `shouldDrawPixel` 的 `alpha > 240` 早退；
- Jet 用 `(x^y)&1` 实现棋盘格奇偶判断（ProtoTracer 的 `GetAlternateX/YIndex` 可简化）；
- `Camera::lookAt` 的分轴 lookAtX/lookAtY（ProtoTracer 相机朝向跟随表情时可参考）。

---

## 7. 不适用 / 需谨慎借鉴的点

1. **RGB565 vs HUB75 RGB888/8bit**：Jet 的帧缓冲格式、HALF_WIDTH 打包（2 列/uint16）依赖 565 屏；HUB75 是每通道 8 bit 的 I2S 流，不能直接套用——迁移的是"半宽/场"的**带宽思想**而非格式。
2. **Z_BUFFERING 默认关闭的取舍**：Jet 在 480×320 关 z-buffer 是为省 150 KB；ProtoTracer 只有 2048 像素，4 KB z-buffer 负担极小，**建议保留逐像素深度**，painter 排序仅作兜底/透明层。
3. **定点数全面替换**：无必要。ProtoTracer 有 FPU 且有 ESP-DSP 批处理；Jet 的定点是为无 FPU 平台与确定性。只摘取 LUT/除法技巧。
4. **Jet 的 `std::vector` 用法**（resize/容量预置）在 ProtoTracer 的 no-alloc 纪律下等效于静态数组/arena；**不要**引入运行时增长的容器。
5. **功能范围差异**：Jet 没有动画引擎、网络、BLE、OTA、传感器——这些仍是 ProtoTracer 的自有壁垒；Jet 经验集中在渲染层。
6. **许可**：两者都是 AGPL-3.0，参考实现合法；若直接复制 Jet 代码需保留版权头并遵守 AGPL 传播义务（产品化时开源）。
7. **场景差异**：Jet 面向"可移动相机的大世界"；ProtoTracer 是"固定相机、小网格、表情动画"——剔除链与 LOD 收益有限，重心应放在 A/C/E/I。

---

## 8. 分阶段建议（与既有计划的关系）

| 阶段 | 内容 | 对应既有计划 | 参照 Jet |
|---|---|---|---|
| S1 测量 | PROFILE 遥测补"三角形/剔除统计"；A/B 编译开关 | Milestone A | `lastFrame*Triangles` 计数 |
| S2 直接光栅化 | 落地增量边函数扫描线光栅化 + 2048 像素 z-buffer；保留 QuadTree 于开关下 | §7.3 Option C | `Renderer.cpp` drawTriangle |
| S3 深度与剔除 | 背面/near-far 剔除；可选 painter 桶排序 | §7.3/§7.5 | `Scene.cpp` emitTri/桶排序 |
| S4 数学微优化 | TrigLUT 替换动画路径 sinf/cosf；float 倒数技巧 | Milestone G | `TrigLUT`/`invDenom64f` |
| S5 特性矩阵 | ImageSequences 按需编译；特性成本表；调试宏 RELEASE 关闭 | §13 | `JetConfig` |
| S6 材质去虚化 | enum 分派 + 逐帧材质预计算 + 快速路径 | §7.5/§8.5 | `Material::getColor`/`flatColorPrecomputed` |
| S7 桌面 harness | 渲染核心抽离 + 桌面回归/基准 | （新增） | 三平台构建 |
| S8 输出优化 | HUB75 SIMD 批量填充；可选隔行渲染实验；M5 预览节流 | §10 | `fillRGB565Span`/`FIELD_BUFFERS` |

> 建议 S2–S4 合并为一个 PR 周期（同一渲染路径重构），S5 可与固件发布并行，S7 长期推进。

---

## 9. 结论

1. **Jet 证实了 ProtoTracer 既有的最大优化方向**（直接三角形光栅化），并提供了可直接对照的量产级实现与性能量级（ESP32-S3 ≈ 40 k tris/s；650 屏幕内三角形 @60fps 480×320）；
2. **ProtoTracer 过去数月的修复（arena 化、预解析、内存放置、双核管线）已把"正确性/内存"地基打好**，当前代码状态优于 2026-05-31 文档所描述；下一步的重心是渲染架构升级与特性矩阵化；
3. **最高性价比的 Jet 迁移项**：增量边函数扫描线光栅化（A）、FAST_Z+桶排序深度策略（C）、背面/near-far 剔除（J）、TrigLUT（D）、材质去虚化与逐帧预计算（I）、编译期特性矩阵（F）、桌面回归 harness（H）；
4. 两项目许可一致（AGPL-3.0）、目标硬件一致（ESP32-S3）、范式互补（应用系统 vs 渲染库），`reference/Jet` 作为常驻参考目录（已 gitignore）可持续跟踪其演进（如 40 k tris/s 数据点的后续优化提交）。

---

## 附录 A：参考文件清单

- Jet（`reference/Jet/`，commit `4db6aa1e`）：README.md、`src/JetConfig.example.hpp`、`src/Renderer.cpp|hpp`、`src/Scene.cpp|hpp`、`src/Camera.cpp|hpp`、`src/Object.hpp`、`src/Math.hpp`、`src/FastMath.hpp`、`src/TrigLUT.hpp|.cpp`、`src/Material.hpp`、`src/Texture.hpp`、`src/Shader.hpp`、`src/Jet.hpp`、`src/Sample.cpp`、`src/ObjLoader.h`、`CMakeLists.txt`、`idf_component.yml`、`library.json`
- ProtoTracer（本仓库）：`src/main.cpp`、`src/Render/{Scene,Camera,Object3D,PixelGroup,QuadTree,Triangle2D,Node}.h`、`src/Math/Mathematics.h`、`src/Controllers/TasESP32S3KitV1.h`、`src/Flash/PixelGroups/P3HUB75.h`、`src/Materials/Material.h`、`src/Animation/{Animation,JsonDrivenProtogenAnimation}.h`、`platformio.ini`、`.gitignore`、`docs/*`

## 附录 B：术语对照

| 术语 | 说明 |
|---|---|
| ray-casting | 逐像素发射光线求交的渲染方式（ProtoTracer 现状） |
| scanline rasterization | 逐三角形扫描线填充的渲染方式（Jet） |
| FAST_Z | 用三角形平均深度代替逐像素深度插值 |
| painter's algorithm | 远→近绘制排序，后画的覆盖先画的 |
| Q10 定点 | 1 个单位 = 1024 的定点数格式 |
| PULP SIMD | ESP32-S3 的 PULP 扩展矢量指令（ee.vst.128 等） |
| field buffer | 奇偶行分两个半高缓冲、逐帧交替渲染（交错） |
| HUB75 | 常见 LED 矩阵面板接口（RGB 双色组 + 行选 + 锁存/使能） |
| ProtoGC | 本项目使用的双堆（内部 RAM/PSRAM）内存管理库 |

---

*报告生成：源码走读 + 双子代理深挖 + 交叉核对；凡"待子代理核对"项见 §3.9 表格后续更新。*
