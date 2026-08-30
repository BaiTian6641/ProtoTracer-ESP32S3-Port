# 面部模型（Morph/形态键）内存优化方案 — 下一步优化计划

> **分支上下文**: `feature/deep-rendering-core-optimization`（渲染核心已完成，render ~20.8→6.0 ms）　|　**日期**: 2026-08-21
>
> **数据来源**: 用 `host-tests/face_analysis.py` / `face_analysis2.py`（penv Python，已留存）对真实 `universal_face.json` 实测。
>
> **目标**: Universal Face 的 morph/形态键数据几乎吃光内存。本文给出可落地的动态内存优化方案，按"性价比 × 风险"排序，并附验证方法。

---

## 0. 摘要（先看这里）

| 方案 | 内容 | 预期节省（新脸 99 morph） | 风险 | 优先级 |
|---|---|---|---|---|
| **A. 按调用集合裁剪 morph** | 只加载"动画配置实际引用"的 morph | **~32 KiB（-46% morph 存储）** | 低（数据驱动扫描） | ★★★ 最高 |
| **B. morph 紧凑存储** | delta 用 half-float、索引用 uint16 | 再省 ~18 KiB（裁剪后再 -50%） | 低-中 | ★★★ |
| **C. 单 PSRAM arena 分配** | 一次大块分配取代逐 morph 小分配 | 消除碎片（不省总量，省碎片） | 低 | ★★ |
| **D. delta 去重表**（可选） | 共享相同 delta 的顶点只存一份 | 视实际重复度（实测 50.9% 共享） | 中 | ★ |
| **E. IndexGroup/顶点索引 uint16** | 三角形索引 int→uint16 | ~3.1 KB | 低 | ★★ |
| **F. 解析期裁剪** | 加载时就跳过未引用 morph 的 vectors | 大幅削减 ~343 KB 解析峰值 | 中 | ★★ |
| **G. cutoff（\|d\|<0.002 剔除整项）** | 剔除"整项都微小"的条目 | 实测仅 0.5%（不建议单独做） | 低 | 见 §5 |

**核心判断（half-float Vector，见 §4）**：**不要**改全局 `Vector3D`（float32）；**只在 morph delta 存储**里用 half-float/紧凑类型，应用时再转 float32。全局改 half 是高风险、影响整个引擎；morph delta 用 half 则数值上完全够、收益明确。

---

## 1. 现状与实测数据

### 1.1 两套脸数据（注意：不一致）

| 文件 | 顶点 | 三角形 | morph 数 | JSON 大小 | 说明 |
|---|---|---|---|---|---|
| `src/Morph/universal_face.json` | 495 | 523 | **99** | 303 KB | 设备实际加载的"新 Universal Face"（串口实测 310269 B） |
| `data/universal_face.json` | 339 | 326 | 85 | 165.7 KB | `buildfs` 打包进 LittleFS 的旧版 |

> ⚠️ **两者不一致**——`data/`（烧录镜像）还是旧脸。本方案以**新脸（99 morph）为准**做测量，同时给出旧脸对照。后续发布需确认以哪套为准并统一。

### 1.2 运行时内存构成（float32 现状）

`JsonNukudeFace::Load` 解析后驻留 PSRAM 的结构：

| 结构 | 大小（新脸） | 说明 |
|---|---|---|
| `vertexBufferStorage` `Vector3D[495]` | 5,940 B | 基础顶点（float32×3） |
| `indexBufferStorage` `IndexGroup[523]` | 6,276 B | 三角形索引（int×3） |
| **morph 数据（99 个）** | **70,400 B（68.8 KiB）** | 每个 morph = `int[vCount]` + `Vector3D[vCount]`（16 B/条目） |
| Object3D 拷贝顶点（modified） | 5,940 B | `TriangleGroup` 拷贝 |
| 渲染双缓冲顶点（render） | 5,940 B | `EnableDoubleBuffer` |
| `Triangle3D` 数组 ×3 | ~95 KB | 523 × ~61 B × 3 份（基础/modified/render） |
| **解析期 JSON doc（临时）** | **~343 KB（用完释放）** | `fileSize + 32768`，PSRAM，解析后释放 |

**morph 存储 68.8 KiB 是本次的主要目标**；解析期 ~343 KB 临时 doc 是 PSRAM 峰值的主要来源。

### 1.3 关键实测：morph 调用集合 vs 存储

新脸 99 个 morph，但**设备实际只会用到一小部分**：

- **实测**：99 个中 **37 个被引用，62 个完全不被调用**（占 morph 存储 **46%**，31.8 KiB 白占）。
- 引用来源（活动动画 = `JsonDrivenProtogenAnimation` + 用户动画 JSON）：
  - 表达式 `anim_parameter` 的 morph 名（如 `Anger/Sadness/Surprised/Happy/EyeNY/...`）；
  - `auto_link`（`HideBlush/HideSecondEye/Zzz/MouthEnd` + 8 个 `vrc_v_*`）；
  - `flipped_morphs`；
  - 代码硬编码：viseme `vrc_v_ss/ee/ih/dd/rr/ch/aa/oh`、眨眼 `Blink/SEyeBlink`、`HideMouth`；
  - 别名：`vrc_v_uh ↔ vrc_v_dd`（`ResolveMorphAlias`）。
- **重要**：morph 引用集合是**数据驱动**的——随用户加载的动画 JSON 变化。因此裁剪必须是**运行时动态扫描**，不能写死。
- 旧脸（85 morph）对照：37 用 / 48 不用（44% 白占，14.4 KiB）。

### 1.4 delta 数值分布（决定 half/量化可行性）

新脸 morph delta 分量（共 13,200 个分量）：

| 指标 | 实测 |
|---|---|
| 非零分量幅度范围 | **[0.00001 … 25.327]**（跨 5 个数量级） |
| 超过 half-float 上限（65504） | 0 |
| 幅度 <0.002（微小） | 4621（35%）——但**分散在各向量的单个分量里** |
| 整项 |max(dx,dy,dz)|<0.002（可整条剔除） | 仅 **20 条（0.5%）** |
| 整项 delta == (0,0,0) | 0 |
| **共享 delta 的顶点（去重空间）** | **2239/4400 = 50.9%** 与其他顶点共享同一 delta；唯一 delta 向量仅 1996 个 |
| 索引连续性 | 新脸仅 2/99 morph 索引连续（密度 0.13）→ **朴素 [start,end] 范围打包不可行** |

---

## 2. 方案 A — 按调用集合裁剪 morph（最高优先级）

**思路**：加载脸之前先扫描"动画配置实际引用的 morph 名"，加载时**跳过未被引用的 morph**，根本不为其分配内存。

**为什么安全**：所有可达 morph 都来自"动画 JSON + 代码硬编码 + 别名"，这三者在加载期都能枚举。被裁剪的 morph 永远不会有非零权重，剔除它们不改变任何视觉输出。

**实现要点**：
1. 新增 `CollectUsedMorphNames()`：从已解析的动画配置收集引用——遍历 `expressions[*].anim_parameter` 的键、`auto_link[*].name`、`flipped_morphs[*]`，并入硬编码集合（`vrc_v_ss/ee/ih/dd/rr/ch/aa/oh`、`Blink`、`SEyeBlink`、`HideMouth`）与别名映射。
2. **加载顺序调整**：当前 `Initialize()` 是 `LoadJsonFaceBlocking()`（先加载脸）→ `LoadAnimationConfig()`（后加载动画）。要让脸加载时已知 used-set，需把"动画配置解析"（只解析、存字符串，不需要脸）提到脸加载之前。注意 `AutoLinkMorphs()` 依赖脸已加载（`GetMorphId` 需要），所以新顺序应为：`LoadAnimationConfig()`（解析）→ `LoadJsonFaceBlocking(usedNames)`（按 used-set 过滤加载）→ `AutoLinkMorphs()` / `LinkParameters()`（在已加载的 morph 中解析 ID）。
3. `JsonNukudeFace::Load` 增加可选的 `usedNames` 过滤参数：遍历 morph 数组时，名字不在 used-set 的直接 `continue`（不分配 idxBuf/vecBuf）。

**风险与对策**：
- 若用户的动画 JSON 引用了某 morph 而我们漏收进 used-set → 该 morph 失效（表情不变）。对策：used-set 收集要覆盖全部引用来源 + 别名；并对"被引用但脸里没有"的名字打日志（现有 `used-but-missing` 检查）。
- BLE `control.set` 切表情只能选已加载配置里的表达式，不会引入新 morph。安全。

**预期**：新脸 morph 存储 68.8 → 37.0 KiB（**省 31.8 KiB，-46%**）。

---

## 3. 方案 B — morph 数据紧凑化（half-float delta + uint16 索引）

**思路**：morph 条目当前是 `int 索引(4B) + Vector3D delta(12B) = 16 B`。改为 `uint16 索引(2B) + 3×half delta(6B) = 8 B`（**-50%**）。

**为什么 half-float 对 delta 数值上安全**（实测支撑）：
- delta 范围 [0.00001 … 25.327]，全在 half-float 范围（±65504）内；
- half-float 精度随数量级自适应：|d|≈25 时步进 ~0.016（视觉无感），|d|≈0.002 时步进 ~2e-6（无损）；
- delta 是**小量修正**，不是大坐标——这点与基础网格/相机坐标（见 §4）本质不同。

**实现要点**：
- 新增紧凑 morph 存储结构（不改 `Vector3D`）：例如 `struct MorphEntry { uint16_t vertexIndex; half dx, dy, dz; }`（或分离的两个数组：`uint16 idx[]` + `half deltas[]`）。
- 提供一个**轻量 half→float 转换器**（符号/指数/尾数位操作，Xtensa 无 half 硬件指令，~20 条指令），仅在 `Morph::MorphObject3D` 应用时转：`vertex += (float)halfDelta * weight`。
- 顶点索引 < 495 → `uint16_t` 足够（同样适用于 `IndexGroup`，见方案 E）。
- 仅对**活跃 morph（Weight>0）**每帧做转换，数量少（每帧通常个位数），转换开销可忽略。

**与方案 D（去重）的关系**：若叠加去重表，delta 表用 half 存，条目存 `uint16 idx + uint16 deltaRef`。见 §6。

**预期**：方案 A 之后 37.0 → **18.5 KiB**（再省一半）。A+B 合计 68.8 → 18.5 KiB（**-73%**）。

---

## 4. 核心判断：是否给 Vector 加 16 位浮点类型？（用户点名要分析）

**结论：不改全局 `Vector3D`；只在 morph delta 存储用紧凑类型。**

| 方案 | 判断 | 理由 |
|---|---|---|
| **全局 `Vector3D` → half-float（16 位）** | ❌ **不建议** | 见下 |
| **morph delta 存储 → half-float（专用类型）** | ✅ **建议** | 数值安全、2× 收益、与渲染管线解耦 |

**全局 `Vector3D` 不建议改 half 的原因**：

1. **引擎级重构风险**：`Vector3D` 被渲染、变换、材质、四元数、法线、UV 插值全链路使用；改类型 = 动整个引擎，且当前渲染刚做到 byte-identical（abDiffs=0），不应为此冒回归风险。
2. **累积精度问题**：half-float 只有 ~3 位有效数字。变换链（矩阵乘加）、向量归一化、点积、LERP 等会**累积**误差；不像静态存储那样误差固定。
3. **大坐标精度**：相机在 z=-500（half 步进 ~0.5），虽然面板投影后每像素 3 单位，单看误差小，但乘法/旋转链会放大。
4. **无硬件加速**：Xtensa 无 half 指令，所有 half 运算都要软件转换，反而可能**变慢**。

**但注意一个意外发现**：实测基础网格坐标其实很小（X∈[-17,11.6]，Y∈[2.8,45.3]，Z∈[-2.4,10.1]），half 步进在 Y=45 处约 0.03 → 投影后约 0.01 像素，**静态存储精度其实够**。所以"基础顶点 half-float"在数值上可行——但它仍属于高风险重构（影响 TriangleGroup/渲染），收益仅 ~6 KB，**列为可选远期项，不进第一阶段**。

> 一句话：**half-float 用在"小而静的存储"（morph delta）上是对的；用在"大而动的计算"（全局 Vector3D）上是错的。**

---

## 5. 方案 E / G — 索引 uint16 与 cutoff 的判断

**方案 E（IndexGroup 与 morph 索引 uint16）**：索引值 < 495 ≪ 65535，`int`(4B)→`uint16`(2B)。
- morph 索引：随方案 B 一并改。
- `IndexGroup`（三角形索引）：523 × (12→6) B = **省 3.1 KB**。改动小、风险低。建议做。

**方案 G（cutoff，|d|<0.002 剔除）——诚实评估：不建议单独做**：
- 实测"整项都微小"的条目只有 **0.5%**（20/4400）。微小的 35% 是分散在各向量的**单个分量**里，不能整条剔除（一个向量 X 微小但 Y 很大）。
- 若要利用这 35%，只能"分量清零"（|d|<0.002→0），这会让更多 delta 变得**相同** → 提升方案 D 的去重率。这是一个**叠加在去重上的有损微调**，视觉无感但属于可选优化。
- **建议**：cutoff 不单独做；如做去重表（D），再把"微小分量清零"作为去重的预处理以提升命中率。

---

## 6. 方案 C / D — 内存布局与去重

**方案 C（单 PSRAM arena，消除碎片）**：
- 现状：`JsonNukudeFace::Load` 对每个 morph 各 `psramAlloc` 两次（idxBuf + vecBuf）→ 99×2 次小分配，容易在 PSRAM 堆里留下碎片。
- 改为：**先按 used-set 计算总字节数，一次性分配一个大 arena，然后向前顺序切分**各 morph 的索引区与 delta 区。
- 对应用户"从后向前寻址避免碎片化"的思路：arena 内可采用**双端布局**——索引区从前往后排、delta 区从后往前排（或反之），中间留空，便于某一侧增长时互不干扰；由于我们预知总量，简单的顺序切分即可，双端布局作为可选增强。
- 解析期 JSON doc（~343 KB）是独立临时分配，解析完即释放，与 arena 生命周期错开，不交织。

**方案 D（delta 去重表，可选/二期）**：
- 实测新脸 **50.9%** 的顶点与其他顶点共享同一 delta（唯一 delta 仅 1996 个）。
- 结构：`half deltaTable[unique]` + 每条目 `uint16 vertexIndex + uint16 deltaRef`。
- 收益估算（仅 used morph）：used 条目 2368，唯一 delta 远小于条目数 → 相比纯方案 B（18.5 KiB）还能再省几 KB，但引入一层间接寻址，且 morph 应用时每顶点多一次表查。**建议放二期，先用 A+B 拿到主要收益**。

---

## 7. 分阶段实施与验证

> 沿用本分支的验证工具链：构建全 env、`host-tests/` 桌面 harness、`RASTER_VERIFY_AB` 设备 A/B 门、COM6 遥测。

| 阶段 | 内容 | 验证 |
|---|---|---|
| M1 | 方案 A：动态 used-set 扫描 + 加载期 morph 裁剪 + 加载顺序调整 | 构建；设备日志打印"loaded morphs=37/99, saved≈31.8KiB"；各表情/viseme/blink/boop 视觉回归（用户目视）；`validate.ps1` |
| M2 | 方案 C：单 arena 分配（含双端布局可选） | 构建；PSRAM 碎片/峰值遥测对比；长跑稳定 |
| M3 | 方案 B：half-float delta + uint16 索引（专用紧凑类型 + half→float 转换器） | 构建；**用 `RASTER_VERIFY_AB` 思路做 morph 等价校验**：加 `FACE_VERIFY` 开关，同帧用 float32 与 half 存储各算一次顶点位置，比较 `abDiffs`（应≈0 或在阈值内）；各表情目视 |
| M4 | 方案 E：IndexGroup uint16 | 构建 + 渲染等价校验 |
| M5（二期，可选） | 方案 D 去重表 + 微小分量清零预处理 | 同上 |

**统一验收指标**（新脸）：
- morph 存储：68.8 KiB → **≤18.5 KiB**（A+B），二期叠加 D 更低；
- 解析期 PSRAM 峰值明显下降（方案 F/A 顺带效果）；
- 视觉与各表情功能零回归（用户目视 + 设备 A/B 校验 abDiffs≈0）；
- 内部 DRAM 不受影响（morph 数据本就在 PSRAM）。

---

## 8. 风险登记 / 注意事项

1. **两套脸不一致**：`data/`（165.7 KB 旧）vs `src/Morph/`（303 KB 新）。方案落地前需统一口径——裁剪逻辑对两者都适用，但发布物要确认以哪套为准。
2. **used-set 完备性是方案 A 的生命线**：漏收一个被引用的 morph = 那个表情永久失效。必须用"配置引用 + 硬编码 + 别名"三路并集，并保留"被引用但脸中缺失"的告警日志。
3. **half-float 只在 morph delta 用**，绝不动全局 `Vector3D`（§4）。
4. **不改变网络/OTA/BLE/启动结构**（延续上一任务约束）。
5. morph 裁剪是**有损于"未引用 morph 的可用性"**的——如果未来某动画 JSON 想用一个当前被裁掉的 morph，需要重新加载（裁剪发生在加载期，换一个引用它的动画配置即可恢复，无需改固件）。

---

## 附：分析工具

- `host-tests/face_analysis.py`：morph 存储/调用集合/零与微小分量统计、used/unused 明细。
- `host-tests/face_analysis2.py`：cutoff 可行性、delta 去重空间、各存储模型字节对比、half-float 范围校验。
- 复跑：`& "C:\Users\weyst\.platformio\penv\Scripts\python.exe" host-tests\face_analysis.py`（penv Python，无额外依赖）。

---

## 实施日志

> 分支 `feature/morph-memory-optimization`（自渲染优化分支切出）。目标：为未来的 BLE/WiFi 远程命令控制等功能腾出内存。设备 COM6 在线 profiling。

### M1 — 按调用集合裁剪 morph（commit `7d1e9e8`，已在 COM6 验证）

- `JsonDrivenProtogenAnimation::CollectUsedMorphNames()`：加载脸之前读取活动动画配置，收集引用的 morph 名（expressions.anim_parameter 键 + auto_link.name + flipped_morphs）+ 硬编码（8 个 viseme、Blink、SEyeBlink、HideMouth）+ 别名（vrc_v_uh↔dd）。**无需调整初始化顺序**（预扫描独立，原 `LoadAnimationConfig→AutoLinkMorphs` 不变）。
- `JsonNukudeFace::Load` 新增可选 `usedMorphNames` 过滤：未引用 morph 直接跳过、不分配。
- **首启安全**：本地无动画配置时（首次下载前）不裁剪、全量加载。
- **设备实测（COM6）**：`[MORPH] morph culling active: 43 used morphs`；脸加载 `morphs=42 skipped=57`；稳态 `psramFree` 从 ~7.68 MB 升到 ~7.96 MB（观测 ~+280 KB；结构账 = morph 存储 68.8→37 KiB，即 -32 KiB，其余为运行期方差 + 分配次数 198→84 的碎片收益）。表情/viseme/眨眼正常、BLE 就绪、无崩溃。
- **排障记录**：一度"无串口输出"实为设备卡在 download 模式（USB-Serial-JTAG 复位采样 GPIO0 低电平），**与固件无关**；正确脱困 = DTR 置高（IO0 释放）+ RTS 脉冲复位。
- 风险确认：活跃路径全部按名访问 morph（`FindMorphIndexByName`→当前索引）；枚举索引式访问只存在于未启用的旧动画类（BetaAnimation/ProtogenHUB75Animation 等），裁剪对活跃路径安全。
