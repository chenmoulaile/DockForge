# T-005 · render: D2D/DWrite 后端 + 材质多级回退

状态：DONE（2026-10-24）
任务：`IRenderBackend` 冻结 + D2D 实现；材质四级回退链；自绘磨砂板；背景捕获线程。

## DoD 核对

| # | 条目 | 结论 | 证据 |
|---|---|---|---|
| 1 | `IRenderBackend` 接口冻结并实现 D2D 版本 | PASS | `src/render/backend.h`、`src/render/backend_d2d.cpp`、`CONTRACTS.md` C-04 |
| 2 | 圆角矩形 + 描边 + 文本绘制可用 | PASS | `alpha96/144/192.txt`：中心 PAINTED、四角 TRANSPARENT；`tier_*.png` 中的标签文本 |
| 3 | 材质回退链按系统版本选择，日志记录实际级别 | PASS | 下表 5 次运行的 `material: requested=... resolved=...` 日志行 |
| 4 | 三级降级均可渲染，不崩溃、不黑块（强制降级开关） | PASS | 四级各自的 `tier_*.png` + `alpha_*` 采样；`DOCKFORGE_FORCE_MATERIAL` |
| 5 | 三张 DPI 截图 | PASS | `dpi96.png` / `dpi144.png` / `dpi192.png` |

## 材质分级实测（`DOCKFORGE_FORCE_MATERIAL` 强制，corner_radius 按档位取值）

配置 `icon_size=96 gap=0 mag=2.50 falloff=sine`，窗口 1056x304 @ 192 DPI（DPI 96/144 时为 528x152 / 792x228）。

| 强制档位 | corner | 解析结果 | 日志降级原因 |
|---|---|---|---|
| `solid` | 40 | `solid` | — |
| `blur` | 40 | `blur` | — |
| `acrylic` | 0 | `acrylic` | — |
| `acrylic` | 40 | **`blur`** | `accent-policy acrylic cannot be clipped to a custom corner radius` |
| `mica` | 0 | `mica` | — |

`os_build=26300`（注册表 `CurrentBuildNumber`；`ProductName` 谎报 Win10，故不采用）。

## 圆角与逐像素透明度（差分采样，`tools/alpha_probe.ps1`）

规则：窗口在 vs 进程被杀后同点采样，差值 ≤2 记为 TRANSPARENT。三档全部 `painted=2 transparent=4`：

| DPI | 物理尺寸 | center | top_mid | 四角 |
|---|---|---|---|---|
| 96 | 528x152 | PAINTED | PAINTED | 全 TRANSPARENT |
| 144 | 792x228 | PAINTED | PAINTED | 全 TRANSPARENT |
| 192 | 1056x304 | PAINTED | PAINTED | 全 TRANSPARENT |

对照（`alpha_solid_96/144/192.txt`）：不透明 solid 档同样 `painted=2 transparent=4`，说明透明来自 DComp surface 的逐像素 alpha，不是采样假象。

## 模糊是真的（棋盘格对比度，`tools/pattern_contrast.ps1`）

8px 黑白棋盘格背景窗置顶（`spike_t005_checker --topmost`），在面板内部矩形 `(992,800,896,60)` 采样相邻像素亮度差：

| 场景 | adjacent_contrast | 判定 |
|---|---|---|
| 对照：原始棋盘格（无 DockForge） | 31.626 | PATTERN_VISIBLE |
| `solid` 档（corner=40） | 2.480 | PARTIAL |
| `blur` 档（corner=40，冻结背景） | **0.029** | **BLURRED** |
| `acrylic` 档（corner=0） | 0.000 | BLURRED |
| `mica` 档（corner=0） | 0.000 | BLURRED |

`blur` 档把 31.6 的对比度压到 0.03（衰减 1000 倍），同时 `luminance_min/max = 63.9/68.9` 仍有 5 的灰度范围——不是纯色填充，是模糊后的均匀霜面。`solid` 档残留 2.48 是 tint alpha 0.92 透出的 8% 背景细节，符合预期。

> 采样矩形必须避开居中的标签文本（近白色 `236,240,248`，会被误判为"背景细节存活"）。首次用全高矩形测得 `1.896 PARTIAL` 就是这个原因，不是模糊失效。

## 背景捕获与差分验证

`BitBlt` 读合成桌面是固定 ~78ms 的合成器往返，与区域大小无关（160x60 到 2560x480 均 77–79ms），必须离开 UI 线程。

差分验证（两块纯色背景，`spike_t005_checker --flat`）：同一块面板的采样随背景变化，证明霜层真的在采样背后像素。

| 背景 | `blur` 档面板 (200,40) | `solid` 档面板 (200,40) |
|---|---|---|
| `rgb(8,8,8)` | 12,13,15 | 14,16,19 |
| `rgb(240,240,240)` | **116,117,119** | 14,16,19 |

`blur` 档随背景亮度从 12 变到 116（约 9 倍），`solid` 档两背景读数完全相同——霜层在采样，实色板不在。

## 性能

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| 帧间隔 p99（blur 档，冻结背景，60s） | 16.54–16.62ms | <16.7ms | PASS |
| 帧间隔 p50 | 16.01ms | — | PASS |
| fps | 62.5 | 60 | PASS |
| 空闲 CPU（blur 档冻结，60s） | 0.094s / 0.157% | <0.5% | PASS |
| 空闲 RSS（blur 档冻结） | 77.06MB | <120MB | PASS |
| 句柄（blur 档冻结，60s） | 294 → 296 | 无增长 | PASS |
| 线程 | 5 → 5 | 稳定 | PASS |
| 实时捕获（200ms 周期，45s） | 句柄 307→306，RSS 102.99→103.11MB | 无泄漏 | PASS |
| 捕获单次耗时 | avg 94.7–96.7ms（稳态） | — | 见下 |

实时捕获模式下 RSS 涨到 ~103MB（比冻结模式多约 26MB，来自持续分配的双缓冲与 DIB 段）。仍在 120MB 预算内，但余量已不足 20MB —— 后续加缩略图（T-015）前必须先解决，已记入 `TASKS.md` T-015 的依赖说明。

捕获耗时从首次 301ms 收敛到稳态 94.7ms，是 `BitBlt` 合成器往返的固有成本，不阻塞 UI 线程（UI 帧时间不受影响）。

## 本次修复的两个缺陷

**1. `SetTransform` 覆盖图集偏移（ADR-0009）**
`PlateRenderer::Draw` 原本写 `dc.SetTransform(Translation(rect.left, rect.top))`，覆盖了 `CompositionSurface::Paint` 设置的图集偏移。症状：96/144 DPI `painted=0 transparent=6`，192 DPI 侥幸正常。修复为先 `GetTransform(&base)` 再 `Translation(...) * base` 复合。

**2. 证据采样被无关窗口污染**
首轮棋盘格测量读到纯色 `12,12,12`，原因是全屏的"设置"窗口盖住了 checker 窗。加 `--topmost` 后对照恢复 31.626。教训已写入工具：`spikes/t005_material/checker.cpp` 的 `--topmost`，以及新增 `tools/window_list.ps1` 用于核对窗口真实矩形与 z 序。

## 证据清单

- `alpha{96,144,192}.txt` — 三档 DPI 差分透明采样（blur 档）
- `alpha_solid_{96,144,192}.txt` — 三档 DPI 差分透明采样（solid 档，几何对照）
- `dpi{96,144,192}.png` — 三张 DPI 截图
- `contrast_raw.txt` / `contrast_solid.txt` / `contrast_blur.txt` / `contrast_acrylic.txt` / `contrast_mica.txt` — 五档对比度
- `tier_{solid,blur,acrylic,mica}.png` — 四档材质截图
- `tier_blur_frozen.png`、`tier_blur_checker.png`、`tier_solid_checker.png` — 过程截图
- `diff_dark.png` / `diff_light.png`、`solid_dark.png` / `solid_light.png` — 差分背景验证截图
- `blur_bench.txt`、`capture_bench.txt`、`capcost.txt`、`dupl_bench.txt` — spike 性能数据
- `run_*.txt`、`px_*.txt`、`shot_*.png` — spike 原始输出（系统材质矩阵与捕获排除实验）

## 遗留

- 实时捕获模式 RSS 103MB，距 120MB 预算余量不足 20MB → T-015 前必须先做捕获内存优化（降采样捕获、复用 DIB、按需暂停）。
- 捕获耗时 94.7ms 是 `BitBlt` 合成器往返的固有限制；若后续需要更低延迟，需另开 spike 验证 `IDXGIOutputDuplication` 在无虚拟适配器的机器上的表现（本机 `E_ACCESSDENIED`）。
- `plate.Draw` 每帧 `CopyFromMemory` 全量上传背景位图；背景未变时可跳过，留待 T-010 与动画优化一并处理。
