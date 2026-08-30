# 内部 SRAM（IRAM + DRAM）占用分析报告

> **目标**: 内部 SRAM 是真正瓶颈（稳态 free 仅 ~21 KB），给未来 BLE/WiFi 远程命令控制腾内部 DRAM 是本报告的目的。
> **数据来源**: (1) `.pio/build/esp32s3-RELEASE/firmware.map` 静态段解析（`host-tests/fw_size_analysis.py`）；(2) 设备 COM6 实测一次性 `SRAM_DUMP`（`-DSRAM_DUMP`，`heap_caps_print_heap_info` + 任务栈高水位）。
> **日期**: 2026-08-21　|　**分支**: `feature/morph-memory-optimization`

---

## 0. 一句话结论

ESP32-S3 内部 SRAM 共 **512 KB**。当前：**IRAM 代码 ~96 KB + DRAM 静态 ~86 KB + 运行时堆已分配 ~261 KB + 空闲 ~20 KB**。真正的问题不只是"剩得少"，而是**碎片化**：空闲 20 KB 里**最大连续块只有 7.7 KB**（`largestBlk=7668`）——任何 >7.7 KB 的**一次性内部 DRAM 分配都会失败**，这正是长跑偶发冻结的根因（ProtoGC 每 5 s 触发 emergency GC 且 reclaim 0）。

---

## 1. 总体布局（实测）

| 类别 | 大小 | 占比 | 说明 |
|---|---|---|---|
| **IRAM 代码**（`.iram0.text`+vectors） | **~96 KB** | ~19% | 需常驻内部 SRAM 的热代码（中断/DMA/热路径），flash 存储、boot 拷入 |
| **DRAM 静态**（`.dram0.data`+`.bss`） | **~86 KB** | ~17% | 全局/静态变量 |
| **运行时堆（内部 DRAM）已分配** | **~261 KB** | ~51% | 任务栈 + BLE/WiFi 运行时 + DMA 描述符 + 渲染暂存 + 各类缓冲 |
| **运行时堆空闲** | **~20 KB** | ~4% | **且最大连续块仅 7.7 KB（碎片化）** |
| 其余（bank 边界/保留/未计） | ~49 KB | ~10% | SRAM0/1/2 bank 划分与对齐开销 |

> 注：HUB75 **像素帧数据走 PSRAM**（`SPIRAM_DMA_BUFFER` 已在 build flags 里设置，`heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM)`），**不占内部 DRAM**——这是已有的好设计。DMA **描述符**（lldesc 链）仍需内部 DMA RAM，但量小。

---

## 2. 静态部分（map 解析）

### 2.1 IRAM（`.iram0.text`，~96 KB，按库）

| 库 | IRAM | 说明 |
|---|---|---|
| esp-idf 系统/中断（`esp-idf-libs` 聚合） | ~34 KB | 中断向量、异常、Cache/PSRAM 管理、RTOS 内核热路径 |
| `libhal.a` | ~7.6 KB | 硬件抽象层（GPIO/timer/SPI 等中断内函数） |
| `libheap.a` | ~5.0 KB | 堆操作（中断安全路径） |
| `libesp_ringbuf.a` | ~3.8 KB | ring buffer（中断内读写） |
| `libesp_driver_rmt.a` | ~2.8 KB | RMT（NeoPixel）驱动 |
| 其余（newlib/esp_mm/i2c/rom/xtensa…） | 各 <1.5 KB | |

> IRAM 主要是 ESP-IDF 框架自己放进去的（中断安全代码）。**固件自研代码几乎不占 IRAM**——目前只有 `Triangle3D::DidIntersect` 用了 `IRAM_ATTR`（可忽略）。

### 2.2 DRAM 静态（`.dram0.data`+`.bss`，~86 KB，按库）

| 来源 | 大小 | 说明 |
|---|---|---|
| **`firmware-src/main.cpp.o`** | **~30.8 KB** | 固件自己的全局对象（见下符号级） |
| `libesp_mm.a` | ~6.9 KB | 内存管理/MMU 状态 |
| esp-idf-libs 聚合 | ~5.2 KB | |
| `liblwip.a` | ~3.8 KB | TCP/IP 状态 |
| `libnet80211.a` / `libphy.a` | ~6.2 KB | WiFi 状态 |
| `libespcoredump.a` | ~2.8 KB | coredump 缓冲 |
| 其余 | 各 <2 KB | |

**main.cpp.o 的 ~30.8 KB 里最大的静态符号**（bss/data，地址在内部 DRAM 区）：

| 符号 | 大小 | 是什么 |
|---|---|---|
| `animation`（bss） | **17,696 B（17.3 KB）** | `JsonDrivenProtogenAnimation` 全局对象（含 morph/表情/材质注册表等容器） |
| `_ZN19MicrophoneFourierIT10fftFiltersE`（bss） | 5,632 B | 麦克风 FFT 滤波器状态 |
| `port_IntStack` | 4,192 B | 中断栈 |
| `gHudBuffer`（bss） | 4,096 B | M5 预览帧缓冲（64×32×2） |
| `g_cnxMgr` / `dns_table` / `s_wifi_nvs` / `s_coredump_stack` / `M5` | 各 1–4 KB | 系统/网络对象 |

> ⚠️ 注意：`P3HUB75`(16 KB)、`lgfx_efont_cn_12`(208 KB)、`NETWIZARD_HTML`(23 KB)、`ELEGANT_HTML`(9 KB)、`bitrev4r_table`(8 KB) 这些**在 flash（rodata），不在内部 DRAM**——它们的地址在 flash 区，不占 RAM。

---

## 3. 运行时堆（设备实测，`SRAM_DUMP`）

稳态（启动完成后）内部 DRAM 堆：

```
free=20360  allocated=260892  minEver=15780  largest_free_block=7668
```

按区域（`heap_caps_print_heap_info`）：

| 区域基址 | 总量 | 空闲 | 已分配 | 块数 |
|---|---|---|---|---|
| `0x600fe000` | 8,152 | 7,760 | 0 | 1 |
| `0x3fce9710` | 22,308 | **32** | 19,800 | 110 |
| `0x3fcf0000` | 32,768 | 5,896 | 26,076 | 4 |
| `0x3fcb1ad0` | **228,416** | 6,672 | **215,016** | **367** |

- 主堆区（`0x3fcb1ad0`，228 KB）已分配 215 KB、**367 个分配块、仅 6 个空闲块**——**严重碎片化**。
- `0x3fce9710` 区（22 KB）几乎用满（仅剩 32 B）。

### 任务栈（13 个任务，hwm = 历史最小空闲，即余量）

| 任务 | 优先级 | 栈余量 hwm | 说明 |
|---|---|---|---|
| esp_timer | 22 | **7,640 B** | 余量很大（栈可缩小） |
| MicFFT | 1 | 5,264 B | 麦克风 FFT 采样任务 |
| nimble_host | 21 | 2,812 B | BLE host（常驻） |
| btController | 23 | 2,772 B | BLE controller（常驻） |
| arduino_events | 19 | 2,860 B | |
| sys_evt | 20 | 2,688 B | |
| tiT（lwIP） | 18 | 2,632 B | TCP/IP 线程（WiFi 关断后应已退出/缩） |
| loopTask | 1 | 2,784 B | 主 loop（渲染+菜单） |
| Tmr Svc | 1 | 2,320 B | FreeRTOS 定时器服务 |
| IDLE0/1 | 0 | 144/344 B | 空闲任务 |
| ipc0/1 | 24 | 88/256 B | 核间 IPC |

> 任务栈本身从堆分配。上表是**余量**（hwm），不是栈总量。栈总量 = 创建时设定值（如动画任务 8192 B、后台下载 8192 B——后台下载任务完成后栈已释放）。

---

## 4. 主要内部 DRAM 消费者归因

1. **BLE 协议栈运行时**（nimble_host + btController 栈 + BLE 堆）——常驻（BLE 遥控要一直在线）。
2. **WiFi/lwIP 运行时**——启动后已关断（`WIFI_OFF`），运行时占用应已回落（好设计，已做）。
3. **渲染管线暂存**（固件自研，可优化）：
   - 相机 ESP-DSP 批处理数组 `tmpX/tmpY/rotX/rotY` = 4×2048×4 = **32 KB 内部 DRAM**（`internalAlloc`）；
   - 直接光栅化 **z-buffer** = 2048×4 = **8 KB 内部 DRAM**（`internalAlloc`）。
   - 合计 **~40 KB** 内部 DRAM 用于渲染暂存。
4. **`animation` 全局对象**（17.7 KB bss）。
5. **DMA 描述符 + 各驱动小缓冲**（零散但多）。
6. **coredump / 中断栈 / esp_timer** 等系统项。

---

## 5. 给 BLE/WiFi 腾内部 DRAM 的建议（按性价比）

| 方向 | 省内部 DRAM | 难度/风险 | 说明 |
|---|---|---|---|
| **A. 渲染暂存移 PSRAM**（`tmpX/Y/rotX/rotY` 32 KB + z-buffer 8 KB） | **~40 KB** | 低-中 | ESP-DSP 可读 PSRAM（经 cache）；每帧一次批量操作，代价是微秒级。`cachedRays` 已在 PSRAM 且正常。**最大单项**。需 `RASTER_VERIFY_AB` 回归 |
| **B. 缩减过大任务栈** | 数 KB | 低 | `esp_timer` 余量 7.6 KB、`MicFFT` 余量 5.3 KB——按 hwm 收紧栈尺寸 |
| **C. 减少小分配/池化**（缓解碎片化） | 不省总量，省碎片 | 中 | largestBlk 7.7 KB 是冻结隐患；把频繁小分配并入 arena/池 |
| **D. `animation` 对象内大容器放 PSRAM** | ~数 KB | 中 | 17.7 KB bss 里的 morph/材质注册表 vector 可移 PSRAM |
| **E. WiFi 彻底关断** | 大（运行期） | 已做 | 启动后 WIFI_OFF 已在做 |

**最高优先级是 A**：仅渲染暂存一项就占 ~40 KB 内部 DRAM，移 PSRAM 后内部 DRAM free 可从 ~20 KB 回到 ~60 KB，直接缓解 `largestBlk` 过小的冻结风险。这正是 `docs/optimization-plan.md` §5.1 内存放置表已有的方向（`cachedRays` 已迁 PSRAM，DSP 暂存数组列为内部——可重新评估）。

---

## 6. 实施记录（方案 A 已落地，commit 见 git）

**方案 A 已在 COM6 验证**：
- 改动：`Camera.h` 的 `EnsureFloatCache`（ESP-DSP 批处理数组 tmpX/Y/rotX/rotY，~32 KB）与 `EnsureZBuffer`（z-buffer，~8 KB）由"内部优先"改为"**PSRAM 优先、内部兜底**"。ESP-DSP 经 cache 读 PSRAM 正常。
- **设备实测（COM6，PROFILE）**：
  - `intFree`：**~20.5 K → ~46.5 K**（翻倍以上）；
  - `largestBlk`：**7668 → 31732**（**4 倍**，碎片化/冻结风险大幅降低）；
  - render：~6.0 → ~7.3 ms（PSRAM 延迟的适度代价；HUB75 由 DMA 独立扫描，对最终显示无感）；
  - 表情/动画正常（RAST 计数不变），无崩溃。
- 内存位置对浮点计算输出无影响（PSRAM 放置输出中性），等价性由既有 `RASTER_VERIFY_AB` 门覆盖（如需回归随时可开）。

---

## 附：复现方法

```powershell
# 静态段（map 解析，含 IRAM/DRAM 分库）
& "C:\Users\weyst\.platformio\penv\Scripts\python.exe" host-tests\fw_size_analysis.py .pio\build\esp32s3-RELEASE\firmware.map
# 运行时堆 + 任务栈（设备一次性 dump，构建加 -DSRAM_DUMP）
$env:PLATFORMIO_BUILD_FLAGS="-DSRAM_DUMP"; pio run -e esp32s3-PROFILE   # 然后烧录 COM6 观察 "INTERNAL SRAM DUMP"
```
