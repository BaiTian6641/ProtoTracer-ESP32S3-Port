# ESP32-S3 端功能概览

本文档总结了 `ProtoTracer-ESP32S3-Port` 仓库中当前实现的主要功能。
内容聚焦于 ESP32-S3 移植版本以及当前代码库已经提供的能力。

## 1. 项目用途

- 这是 `ProtoTracer` 3D 渲染与动画引擎的 ESP32-S3 移植版本。
- 该项目面向微控制器上的实时渲染，支持像素矩阵和面罩显示。
- 这个移植版面向 ESP32-S3 / ESP32-P4 硬件，使用 PSRAM、LittleFS、BLE、Wi-Fi 和 HUB75 输出。

## 2. 构建目标与 PlatformIO 环境

在 `platformio.ini` 中配置了以下环境：

- `env:esp32s3` - 面向 `esp32_s3_n8r8` 的调试构建。
- `env:esp32s3-RELEASE` - 面向 `esp32_s3_n8r8` 的发布构建，启用了 token 认证和远程配置能力。
- `env:esp32p4` - 备用的 ESP32-P4 构建目标。

启用的构建特性包括：

- `TASESP32S3` 和 `CONFIG_IDF_TARGET_ESP32S3`，用于 ESP32-S3 专用代码。
- `BOARD_HAS_PSRAM` 以及 `CONFIG_SPIRAM_*` 相关标志，用于外部 PSRAM，承载大 JSON 和渲染缓冲区。
- `ARDUINO_USB_CDC_ON_BOOT=1`，用于 USB 串口支持。
- `USE_PSRAM_FOR_FACE_JSON`，将面部模型 JSON 解析放入 PSRAM。
- `NETWIZARD_USE_ASYNC_WEBSERVER=1` 和 `ELEGANTOTA_USE_ASYNC_WEBSERVER=1`，用于异步 Web 服务。

## 3. 核心运行入口

### `src/main.cpp`

- 初始化 Wi-Fi、显示屏、BLE、手势传感器、文件系统以及渲染管线。
- 根据构建标志选择控制器实现：
  - ESP32-S3 使用 `TasESP32S3KitV1`。
  - ESP32-P4 使用 `TasESP32P4KitV0`。
- 创建主要运行对象：
  - `JsonDrivenProtogenAnimation animation`
  - `AsyncWebServer server`
  - `UserConfig userConfig`
  - `FaceUpdateConfig faceUpdateConfig`
- 设置远程文件同步和 HTTP 中继路由，用于资源下载。
- 在可用时使用 ElegantOTA 注册 OTA 门户路由。
- 处理 OTA 按键状态以及运行循环调度。

## 4. 显示与输出硬件

### 本地显示

- 使用 `M5UnitGLASS2` 显示设备状态、进度、图标和二维码。
- 本地显示相关代码分布在 `main.cpp`、`src/Menu/ESPMenu.h` 和动画类中。

### LED 矩阵输出

- `src/Controllers/TasESP32S3KitV1.h` 通过 `ESP32-VirtualMatrixPanel-I2S-DMA` 实现 HUB75 输出。
- 支持由 `PANEL_RES_X`、`PANEL_RES_Y`、`NUM_ROWS`、`NUM_COLS` 配置的 `64x32` 面板模块链路。
- `virtualDisp` 负责将像素渲染到矩阵面板，同时镜像到 M5 显示屏用于本地预览。
- 控制器基类行为位于 `src/Controllers/Controller.h`。

### 像素指示灯

- 使用单颗 `Adafruit_NeoPixel` LED 作为状态指示。

## 5. 动画引擎

### 基础动画框架

- `src/Animation/Animation.h` 定义了通用的 `Animation<numObjects>` 基类。
- 它提供：
  - `GetScene()`
  - `GetAnimationTime()`
  - `FadeIn()`、`FadeOut()`、`Update()` 抽象接口
  - `UpdateTime(ratio)` 的时间更新封装

### JSON 驱动动画

- `src/Animation/JsonDrivenProtogenAnimation.h` 是主动画实现，负责面部渲染。
- 它从 `LittleFS` 读取动画元数据：
  - `<device_id>_animation.json`
  - 备用 `/example_animation.json`
- 支持基于 JSON 的以下内容：
  - 来自 `JsonNukudeFace` 的面部模型和 morph target
  - 材质选择与动画材质
  - 场景效果（模糊、抗锯齿、频谱分析、彩虹噪声/螺旋）
  - 表情映射、眨眼跟踪、语音检测、boop 交互
- 在启用时使用 PSRAM 友好的 JSON 分配方式。
- 内含的动画对象包括：
  - `RainbowNoise`、`RainbowSpiral`、`GradientMaterial`、`SimpleMaterial`
  - `SpectrumAnalyzer`、`BlinkTrack`、`FFTVoiceDetection`
  - 屏幕空间效果：`HorizontalBlur`、`VerticalBlur`、`RadialBlur`、`AntiAliasingEffect`
- 可配置行为包括眨眼/语音开关、嘴部和眼型切换、色相偏移绑定以及自动链接规格。

### 其他动画类

- `src/Animation/BetaAnimation.h`
- `src/Animation/ClockAnimation.h`
- `src/Animation/CoelaBonkAnimation.h`
- `src/Animation/GammaAnimation.h`
- `src/Animation/ProtogenHUB75Animation.h`
- `src/Animation/ProtogenHUB75AnimationSplit.h`
- `src/Animation/TasSimpleProtogenHUB75Animation.h`

这些类提供了备用动画预设、时钟演示以及 HUB75 专用渲染示例。

## 6. 输入与用户交互

### 手势 / 接近传感器

- 支持两种手势传感器驱动：
  - 定义 `NEW_GESTURE` 时使用 `PAJ7620`。
  - 否则使用 `APDS9960`。
- 手势和接近检测用于 boop 触发和菜单交互。
- 实现在 `src/Menu/ESPMenu.h`。

### BLE 远程控制

- `src/Menu/ESPMenu.h` 实现了一个 BLE UART 服务，包含：
  - BLE 服务 UUID `73cf57c7-6797-46e8-8202-dc5e7f956b57`
  - 可由 `UserConfig` 配置的 RX/TX 特征 UUID
- BLE 同时支持：
  - JSON 命令协议（`ApplyBleJsonWrite`）
  - 进入 `remoteCommandQueue` 的旧式原始控制命令
- 通过 BLE 回传远程状态、设备信息和清单响应。
- BLE 还可远程设置亮度、表情、语音检测和显示模式。

### 机身菜单状态

- `src/Menu/ESPMenu.h` 中的 `Menu` 类保存 UI 状态：
  - 亮度、强调亮度、麦克风状态、表情索引
  - 频谱镜像、颜色、脸部大小、语音启用状态
  - BLE 连接状态和 boop 传感器状态
- `Menu::Update()` 会刷新 M5 显示屏上的状态图标和控制信息。

## 7. 网络与远程资源

### 用户配置管理

- `src/Network/UserConfigManager.h` 负责：
  - 在 LittleFS 中加载 / 保存 `UserConfig`
  - 读取设备唯一 ID
  - 使用保存的 Wi-Fi 凭据连接网络，或进入 NetWizard 门户
  - 从 GitHub 或 Gitee 下载远程 `user_config.json`

### 远程文件同步

- `src/Network/RemoteFileSync.h` 提供：
  - 基于 MD5 的变更检测
  - 多个远程源之间的故障转移
  - 文件下载和本地缓存管理
  - 可选的 `M5UnitGLASS2` UI 提示

### 固件更新

- `src/Network/FirmwareUpdater.h` 支持：
  - 远程固件清单检查
  - 主 / 备源选择
  - 版本比较和 OTA 更新执行

### 面部模型更新

- `src/Network/FaceModelUpdater.h` 用于确保正确的 face JSON 存在：
  - 设备专用面部模型或通用备用 face JSON
  - 远程下载和本地缓存

## 8. 渲染与场景管理

### 控制器架构

- `src/Controllers/Controller.h` 是抽象渲染控制器。
- 它管理摄像机数组、渲染耗时、亮度软启动以及场景效果。
- 派生控制器需要实现 `Initialize()` 和 `Display()`。

### ESP32-S3 矩阵控制器

- `src/Controllers/TasESP32S3KitV1.h` 使用 I2S DMA 驱动 HUB75 面板：
  - 配置 RGB、行选择、时钟、锁存和 OE 引脚
  - 分配 `MatrixPanel_I2S_DMA`
  - 创建 `VirtualMatrixPanel` 用于像素渲染
  - 将渲染结果复制到本地 M5 显示屏镜像

## 9. 外部依赖

PlatformIO 使用的库包括：

- `Adafruit GFX` / `Adafruit SSD1306` / `Adafruit BusIO`
- `NTPClient`
- `Adafruit BNO055` / `Adafruit Unified Sensor`
- `ArduinoFFT`
- `ESPAsyncWebServer` / `AsyncTCP`
- `ArduinoJson`
- `Adafruit NeoPixel`
- `ESP32-HUB75-MatrixPanel-DMA`
- `QRCode`
- `SparkFun APDS9960` 或 `RevEng_PAJ7620`
- `M5GFX`、`M5Unified`
- `NetWizard`
- `ElegantOTAPro` / `ElegantOTA`

## 10. 存储与文件系统

- 使用 `LittleFS` 存放本地 JSON 资源和配置文件。
- 支持以下文件类型：
  - 面部模型 JSON 文件
  - 动画 JSON 文件
  - `user_config.json`
  - 远程清单缓存

## 11. 已知问题与限制

来自 `README.md`：

- 长时间运行时可能出现内存问题
- 频谱分析器当前不可用，需要重写 DSP 部分
- ESP-NOW 远程控制器已标记为过时，建议改用 BLE
- 在这个移植版中，APDS9960 已被 PAJ7620 替代

## 12. 关键源代码目录

- `src/main.cpp` - 启动、运行循环、Web 服务器、OTA、配置加载。
- `src/Animation/` - 动画引擎、JSON 驱动的面部 / 动画、预设。
- `src/Controllers/` - 显示控制器与 HUB75 硬件抽象。
- `src/Menu/` - BLE 菜单、手势交互、屏幕 UI 状态。
- `src/Network/` - 远程配置、资源同步、固件和面部更新。
- `src/Flash/` - 图标、材质、板载 Flash 资源。
- `src/Render/` - 场景、摄像机、物体、材质渲染基础对象。
- `src/Sensors/` - 麦克风和传感器前端，用于语音和 boop 检测。

## 13. 推荐的开发入口

- `src/main.cpp` - 理解移植版的启动流程。
- `src/Animation/JsonDrivenProtogenAnimation.h` - 理解动画和面部渲染管线。
- `src/Menu/ESPMenu.h` - 理解 BLE、手势和菜单输入。
- `src/Controllers/TasESP32S3KitV1.h` - 理解 HUB75 输出和矩阵显示。
- `src/Network/UserConfigManager.h` - 理解远程配置和 Wi-Fi 引导流程。

---

本概览旨在帮助你快速识别 ESP32-S3 移植版已经实现了哪些功能，以及对应代码位于哪里。