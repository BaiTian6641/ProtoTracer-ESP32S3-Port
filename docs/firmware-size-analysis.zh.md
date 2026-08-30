# 固件体积分析报告（生成产物）

> **分析对象**: `.pio/build/esp32s3-RELEASE/firmware.elf` + `firmware.map`（RELEASE 环境，2.25 MB 构建产物）
> **工具**: `xtensa-esp32s3-elf-size/-nm/-objdump` + 自写 map 解析器 `host-tests/fw_size_analysis.py`
> **日期**: 2026-08-21　|　**分支**: `feature/morph-memory-optimization`

---

## 0. 一句话结论

固件（写入 flash 的部分）约 **2.0 MB**（代码 1.26 MB + 只读数据 0.62 MB + IRAM 代码 96 KB）。**最大的单一内容不是图像序列，而是 ESP-IDF 的连接协议栈（WiFi+BLE+TCP/IP+TLS，合计 ~600 KB 级）**，其次是 **M5GFX 中文字体（208 KB）** 和固件自研代码（~200 KB）。**`ImageSequences`（每个 3–4 MB 的 13+ 个序列头文件）和编译期内嵌旧脸（`UniversalFace.h`，112 KB）当前都没有被链接进固件**（链接器 --gc-sections 回收了未引用部分）。

> ⚠️ **关键区分**：flash（代码存储）与 RAM（运行时内存）是两回事。flash 用了 2.0/3.34 MB（60%），**还有 ~1.3 MB 富余**，不是瓶颈；真正紧张的是**内部 DRAM**（运行时堆，实测稳态 free 仅 ~21 KB）。固件体积优化省的是 flash；给 BLE/WiFi 扩展腾地方主要是省**内部 DRAM**。见 §5。

---

## 1. 段级分解（实际写入 flash / 占用 RAM 的部分）

| 段 | 大小 | 含义 | 落点 |
|---|---|---|---|
| `.flash.text` | 1,322,900 B（1.26 MB） | 代码（XIP 自 flash 执行） | flash |
| `.flash.rodata` | 654,320 B（0.62 MB） | 只读常量（表、字符串、字体、内嵌 HTML） | flash |
| `.flash.rodata_noload` | 23,251 B | 只读数据（不加载副本） | flash |
| `.iram0.text` | 98,795 B（96 KB） | 需常驻 IRAM 的热代码（中断/DMA/热路径） | flash 存储→boot 拷入 IRAM |
| `.dram0.data` | 24,720 B（24 KB） | 已初始化全局/静态变量 | 内部 DRAM |
| `.dram0.bss` | 61,752 B（60 KB） | 零初始化全局/静态（含 `animation`、`gHudBuffer` 等） | 内部 DRAM |

**未计入 flash 的**：`.debug_*`（17 MB+，纯调试信息，不烧录）；`.ext_ram.dummy`（2 MB）/`.flash_rodata_dummy`（1.37 MB）是 XIP-from-PSRAM 的**占位区**，不是真实内容。

构建自报：`Flash: 67.4% used 2252515 / 3342336 bytes`；`RAM: 1.0% used 85368 / 8921088`（后者是**静态** RAM，非运行时堆）。

---

## 2. 模块级大头（map 聚合，按写入 flash 的字节）

> 注：个别 object 的归属在解析器里有误差（如 Wire），以**库级聚合**为准。

| 子系统 | 约大小 | 说明 |
|---|---|---|
| **ESP-IDF WiFi 协议栈** | **~298 KB** | `libnet80211`(161K)+`libpp`(53K)+`libphy`(29K)+`libwpa_supplicant`(55K) |
| **M5GFX 中文字体** | **~208 KB** | `lgfx_efont_cn_12`（详见 §3） |
| **BLE 协议栈** | **~160 KB** | `libbt`(91K)+`libbtdm_app`(69K) |
| **lwIP（TCP/IP）** | **~125 KB** | `liblwip.a` |
| **mbedTLS（HTTPS 加密）** | **~138 KB** | `libmbedcrypto`(98K)+`libmbedtls_2`(40K) |
| **固件自研代码** | **~200 KB** | `main.cpp.o`(165K)+`Network/`(34K) |
| **工具链运行库** | ~60 KB+ | `libc.a`(53K, 含 printf/scanf)+`libstdc++/libgcc` |
| **LittleFS** | ~28 KB | `libjoltwallet__littlefs.a` |
| **M5GFX/M5Unified 显示驱动** | ~30 KB+ | `M5GFX.cpp.o`(15K)+面板/总线若干 |
| **NetWizard** | ~37 KB | `nwp.cpp.o`(23K)+`NetWizard.cpp.o`(15K) |
| **ESPAsyncWebServer/AsyncTCP/HTTPClient** | ~40 KB | Web 服务器 + HTTP 客户端 |

**连接协议栈合计（WiFi+BLE+TCP/IP+TLS）≈ 600 KB 级，是 flash 的第一大户**——这是"远程资产同步 + OTA + BLE 遥控 + 配网门户"的代价。

---

## 3. 符号级大头（单个最大的内容项）

| 符号 | 大小 | 是什么 |
|---|---|---|
| `lgfx_efont_cn_12` | **213,444 B（208 KiB）** | **M5 小屏的中文字体表**（`display->setFont(&fonts::efontCN_12)`）——固件里最大的单一数据 |
| `NETWIZARD_HTML` | **23,117 B（22.6 KiB）** | NetWizard 配网门户的**内嵌 HTML 页面** |
| `animation`（bss） | 17,696 B | `JsonDrivenProtogenAnimation` 全局对象（内部 DRAM bss） |
| `P3HUB75` | **16,384 B（16 KiB）** | 面板像素坐标表（2048×Vector2D，flash） |
| `ELEGANT_HTML` | **9,039 B（8.8 KiB）** | ElegantOTA 的**内嵌 OTA 网页** |
| `bitrev4r_table_4096_fc32` | 8,064 B | ESP-DSP 的 FFT 位反转表 |
| `_ZN19MicrophoneFourierIT10fftFiltersE` | 5,632 B | 麦克风 FFT 滤波器表 |
| `gHudBuffer`（bss） | 4,096 B | M5 预览帧缓冲（64×32×2，内部 DRAM） |

**最大的几个函数（代码）**：libc 的 `_svfprintf_r`/`_vfprintf_r`/`__ssvfiscanf_r`（printf/scanf 系列，**合计 ~30 KB**）；`M5GFX::autodetect`（7.4 KB）；`LoadAnimationConfig`（7.8 KB）；`setup()`（8.1 KB）；`mbedtls_ssl_handshake_*`（TLS 握手）。

---

## 4. 明确**没有**占空间的（纠正常见误解）

- **`src/Flash/ImageSequences/*.h`**（BadApple/Wave/Rick 等 13+ 个，每个 3–4 MB）：**当前一个都没链接进固件**（map 中无任何相关符号）。它们仅在被子系统引用时才会被编译；当前未被引用 → 链接器回收。**它们不是当前固件膨胀的原因**。
- **编译期内嵌旧脸 `UniversalFace.h`（112 KB，`Morph morphs[83]` + 大量静态 `*Vectors/*Indexes` 表）**：**未被链接**（活跃路径用的是 JSON 加载的 `JsonNukudeFace`，内嵌 NukudeFace 未被实例化 → 静态表被回收）。
- `.debug_*`（17 MB）只在 ELF 里用于调试，**不烧录**。

---

## 5. 针对"留空间给未来 BLE/WiFi 功能"的判断

flash 还有 ~1.3 MB 富余（60% 占用），所以**flash 不是瓶颈**；真正紧的是**内部 DRAM**（运行时堆，实测稳态 free ~21 KB，ProtoGC 每 5 s 触发一次 emergency GC）。给 BLE/WiFi 扩展腾地方，优先级应从"省内部 DRAM"角度排：

| 方向 | 省什么 | 大小 | 难度/风险 |
|---|---|---|---|
| **WiFi 用后彻底关断**（已做：启动后 `WIFI_OFF`） | 运行时内部 DRAM（WiFi 缓冲区/任务栈） | 大（运行期） | 已做 |
| **morph/face 紧凑化**（本分支 M1/M3/M4） | PSRAM（间接腾出总预算） | ~54 KB（结构） | 已完成 |
| **M5 中文字体**（208 KB flash） | flash；若 M5 屏不显示中文/改子集化字体 | 208 KB flash | 低（改字体引用） |
| **内嵌 HTML 外移**（NetWizard 22.6 KB + ElegantOTA 9 KB） | flash；改从 LittleFS 读 | ~31 KB flash | 中 |
| **printf/scanf 精简**（少用浮点 %f、或轻量 printf） | flash（~30 KB）+ 少量 IRAM | ~30 KB flash | 中（工具链层） |
| **解析期 JSON doc 流式化**（face 343 KB / anim 峰值） | **PSRAM 峰值**（启动期） | 峰值大 | 中 |
| **渲染网格 `Triangle3D` ×3 份**（~95 KB×3） | 内部 DRAM/PSRAM 取决于放置 | 大 | 高（触碰渲染） |

**若要下一步专门给 BLE/WiFi 腾内部 DRAM**，建议先做一次**运行时内部 DRAM 分解**（各任务栈 + 堆占用），而不是只看 flash——可以另起一轮用 `heap_caps_get_free_size`/`uxTaskGetStackHighWaterMark` 做运行时画像。

---

## 附：复现方法

```powershell
# 段级分解
& "C:\Users\weyst\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp32s3-elf-size.exe" -A .pio\build\esp32s3-RELEASE\firmware.elf
# 模块/库级聚合（自写解析器）
& "C:\Users\weyst\.platformio\penv\Scripts\python.exe" host-tests\fw_size_analysis.py .pio\build\esp32s3-RELEASE\firmware.map
# 符号级（最大符号）
& "...\xtensa-esp32s3-elf-nm.exe" --print-size --size-sort --radix=d .pio\build\esp32s3-RELEASE\firmware.elf
```
