# T-008 验证报告 · dock: 布局引擎 + fisheye 数学（纯逻辑）

- 任务: T-008
- 完成时间: 2026-10-24
- 契约: `CONTRACTS.md#C-05`、ADR-0004
- 产出: `src/dock/layout.h`、`src/dock/layout.cpp`、`tests/layout_test.cpp`、`tools/layout_bench.cpp`

## DoD 逐条核对

| # | DoD | 结果 | 证据 |
|---|---|---|---|
| 1 | 实现 C-05 公式：`s_i`、累积位移、边界钳制 | PASS | `src/dock/layout.cpp`；`prefix-sum layout matches the literal O(n^2) contract` |
| 2 | 单测：鼠标在 Dock 外 / 图标数 0,1,4,20 / 极端 magnification | PASS | 15 个用例，见下 |
| 3 | 单调性：鼠标右移时缩放峰单调右移 | PASS | `the peak follows the pointer and never goes backwards` |
| 4 | 100 万次求值耗时 < 20ms（纯 CPU 参考实现） | PASS | `bench.txt`：5.26–7.04ms |
| 5 | 输出 `evidence/T-008/bench.txt` | PASS | 本目录 |

## 契约实现说明

C-05 的累积位移写成双重求和是 O(n²)：

```
x_i = base_i + Σ_j (s_j - 1) * w * 0.5 * sign(i - j)
```

因为 `sign(i - j)` 只取决于 j 在 i 的哪一侧，求和可拆成「左侧全部减右侧全部」，两者都是前缀和，O(n) 即可。这个等价关系**不是口头声称**——`tests/layout_test.cpp` 里的 `ReferenceLayout` 是契约的逐字转写（含 O(n²) 双循环），测试对 `count ∈ {1,2,4,7,20}` × `mouse ∈ {0, 37.5, 150, 999}` 共 20 组逐项比对 `scale`（margin 1e-5）、`center`（1e-3）、`width`（1e-4）。

三条曲线（`Sine` / `CosineIntegral` / `Gaussian`）在 `t = 1` 处都必须恰好归零，否则影响半径边界会出现可见台阶。Gaussian 需要显式归一化：

```cpp
return std::clamp((raw - edge) / span, 0.0f, 1.0f);   // raw = exp(-(p*t)^2), edge = exp(-p^2)
```

未归一化时 `p = 2` 在 `t = 1` 处残留 `exp(-4) ≈ 0.0183`，即约 1.5% 的缩放跳变。`falloff weight is continuous at the influence radius for every curve` 锁定三条曲线在该点都 `< 1e-6`。

## 两处实现缺陷（由测试与 benchmark 分别暴露）

### 1. 指针钳制区间错位（单测暴露）

初版把指针钳到 `[0, base_width]`。第一个图标的中心在 `w/2`，所以钳制下界会把指针钉在图标**外边缘**，离中心半个宽度——边缘图标峰值只能到 `1 + (k-1)*(1 - 0.5/R)^p`，永远达不到 `magnification`。

`pointer far outside the dock still magnifies the edge icon` 断言 `peak_scale == magnification` 时失败：钳到 `[0, base_width]` 后 6 图标 Dock 的 `base_width = 328`，首图标中心 24，距离 24px，`t = 24/144 = 0.1667`，`s = 1 + 0.8 * (1 - 0.1667)^2 = 1.5556`，而期望 1.8。改为钳到首尾图标**中心**的跨度：

```cpp
const float half = w * 0.5f;
mouse = std::clamp(mouse, half, std::max(half, base_width - half));
```

这个钳制同时是单调性的前提：鼠标右移时峰值索引不减。

### 2. 热路径每次分配（benchmark 暴露）

初版每次 `LayoutDock` 都 `std::vector<float> gain(writable)`，与头文件「allocation-free so it can run inside the frame loop」的承诺矛盾，也违反 C-05 的「禁止逐帧重建布局对象」。

benchmark 首轮数字：`48px gap8 mag1.8 n=10` → 129.88ms / 1e6 次。去掉分配后 80.96ms。再对半径外的图标跳过 `pow()`（20 图标里通常只有 6 个在影响半径内）后降到 7ms 量级。最终 gain 从 `out[i].scale - 1.0f` 读回，完全不需临时数组。

## benchmark 单位（重要）

首版把「一次求值」当成一次完整 Dock 布局，判 FAIL。C-05 的求值单位是**单个图标的公式项**（scale + 位移），不是整条 Dock。修正后同时输出两个口径，避免用有利单位掩盖帧成本：

| 场景 | 1e6 图标求值 | 单图标 ns | 单次整条 Dock ns |
|---|---|---|---|
| 48px gap8 mag1.8 n=10 | 7.012ms | 7.01 | 70.1 |
| 96px gap0 mag2.5 n=20 | 7.041ms | 7.04 | 140.8 |
| 48px gap8 mag1.8 n=40 | 5.260ms | 5.26 | 210.4 |
| 32px gap32 mag1.0 n=8 | 5.572ms | 5.57 | 44.6 |
| ComputeScale only | 5.334ms | 5.33 | — |

最坏整条 Dock 成本 210ns，占 16.7ms 帧预算的 0.0013%。

## 测试

```
All tests passed (1316 assertions in 87 test cases)
```

T-008 新增 15 个用例（`[layout]` 标签），覆盖：

- `scale is exactly magnification at the pointer and exactly 1 at the radius`
- `scale is monotonically non-increasing in distance`
- `falloff weight is continuous at the influence radius for every curve`
- `prefix-sum layout matches the literal O(n^2) contract`
- `no hover means resting geometry: scale 1, even spacing, no shift`
- `empty dock is not a special case that crashes`（count = 0 与 -3）
- `a single icon magnifies and does not move`
- `the peak follows the pointer and never goes backwards`
- `pointer far outside the dock still magnifies the edge icon`（±4000px）
- `adjacent icons never change order and never jump`（1px 步进）
- `extreme magnification stays finite and ordered`（mag 2.5 / sharpness 4 / radius 5）
- `magnification below 1 is clamped to resting size`
- `base width and icon count round-trip`（n = 1..30）
- `FromConfig scales DIP to device pixels`（1.0/1.5/2.0）
- `hover width covers the drawn extent, not just the resting row`

## 边界与已知限制

- **本任务不含 GPU 变换**。C-05 要求「缩放与位移只在顶点缓冲/变换矩阵中体现」，那是 T-010 的交付物；T-008 交付的是产生这些数值的纯 CPU 参考实现，benchmark 标题明确写 `纯 CPU 参考实现`。
- 布局模块无 Win32 依赖，可在无窗口进程内测试，这是它被做成纯函数的原因。
- `IconCountForWidth` 只做几何容量计算，不涉及实际取多少个图标（那是 T-009 的职责）。

## 复现

```powershell
& .\tools\build.ps1 -Action configure
& D:\CMake\bin\cmake.exe --build build -j 8
& .\build\bin\dockforge_tests.exe
& .\build\bin\layout_bench.exe
```
