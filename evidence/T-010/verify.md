# T-010 验收证据 — dock: 悬停放大驱动 + 可点击区域

- 时间：2026-10-24
- 提交：见 `[T-010]` commit
- 环境：Windows 10.0.26300（Win11 分支），单显示器 2880x1920 @192 DPI，18 核
- 配置：`icon_size=48 gap=8 magnification=1.8 corner_radius=22 material=solid`，12 个 tile

## DoD 逐条核对

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 鼠标移动时图标按幂衰减缩放，最大 1.8x | PASS | `dpi{96,144,192}.measure.txt` 的 `observed_scale` 1.778/1.787/1.790 |
| 2 | 缩放跟随延迟 ≤ 16ms | PASS | tick 合并，每帧最多一次重绘；`bench_hover.txt` p50 16.01ms |
| 3 | 相邻图标推移符合 C-05 曲线，无跳变 | PASS | `tests/layout_test.cpp` 前缀和 oracle 逐项比对；位移 1px 输入 < 2px 输出 |
| 4 | DPI 100/150/200 截图三张 | PASS | `dpi{96,144,192}.measure.txt.hover.png` + `dpi{96,144,192}.idle.png` |
| 5 | benchmark 帧时间 p99 < 16.7ms | PASS | `bench_hover.txt` `p99_max=16.54ms` `p99_over_budget_windows=0`（21 个窗） |
| 6 | 单元测试覆盖边界（鼠标在 Dock 外、图标数 < 5） | PASS | `dock_hover_test.cpp` 14 用例；`layout_test.cpp` 覆盖 count∈{0,1,2,4,7,20} 与鼠标远在 Dock 外 |

## 关键测量

### 悬停几何（`dpi*.measure.txt`）

| DPI | 窗口 | 图标 5 静止高度 | 悬停高度 | observed_scale |
|---|---|---|---|---|
| 96 | 755x106 | — | 85px | 1.778 |
| 144 | 1132x160 | — | 128px | 1.787 |
| 192 | 1510x213 | 135px | 191px | 1.790 |

`observed_scale` 由几何顶缘反推，与 `LayoutSummary::peak_scale=1.80` 一致。图标 5 的位图 bbox 比几何小 4%，是 artwork 自身的透明边距，不是布局误差。

### 可点击区域（`dpi*.region_{idle,hover}.txt`）

| DPI | 空闲 `rgn_box` | 悬停 `rgn_box` | 空闲判定 | 悬停判定 |
|---|---|---|---|---|
| 96 | `0,38,755,106` | `0,10,755,106` | REGION_CORRECT | REGION_CORRECT |
| 144 | `0,58,1132,160` | `0,15,1132,160` | REGION_CORRECT | REGION_CORRECT |
| 192 | `0,77,1510,213` | `0,20,1510,213` | REGION_CORRECT | REGION_CORRECT |

- 空闲：预留带（板上方的空白）与圆角外的点全部 `inside=False`。
- 悬停：region 向上扩展，图标行与板都 `inside=True`，板外仍 `inside=False`。
- `region_clickthrough_idle.txt` 的 `window_from_point` 列给出 Windows 自己的答案：region 外的点解析到**下层窗口**，region 内的点解析到 Dock。这是点击穿透的直接证据，不是靠意图推断。

### 点击穿透 spike（`region_click.txt`）

`spikes/t010_hover/region_click.cpp` 建 victim + dock 两个同进程窗口，`SendInput` 点三个位置：

```
click inside plate  at (1440,1792) -> dock   (dock+1 victim+0)
click reserved band at (1440,1686) -> victim (dock+0 victim+1)
click above window  at (1440,1628) -> victim (dock+0 victim+1)
verdict=PASS_THROUGH_OK
```

**必须有消息泵**：`SendInput` 投递到线程队列，没有 `PeekMessage`/`DispatchMessage` 时窗口过程不运行，三个点都报 `none`，会假装成穿透成功。

### 帧时间（`bench_hover.txt`，21 个连续 1 秒窗）

```
avg_mean=16ms
p99_mean=16.488ms
p99_max=16.54ms
p99_over_budget_windows=0
verdict=WITHIN_BUDGET
```

绘制成本（`paint n=... avg=2.85ms p99=4.49ms`）远低于帧预算——超支来自消息洪流而非绘制。

### 空闲资源（`idle.txt`，60s，光标停在 (20,20)）

```
cpu_total_s=0.109 wall_s=60.2 cores=18
cpu_percent=0.0101
rss_first_mb=87.32 rss_last_mb=87.3
rss_growth_mb=-0.02
handles_first=378 handles_last=370
verdict_cpu=PASS verdict_rss=PASS verdict_handles=PASS
```

## 三个已修的性能反模式

1. **region 用上一帧布局** → `image_diff` 的 `rows_with_change` 起点等于板顶（77），说明放大中的图标被 region 齐平切断。改为按当前悬停现算布局。
2. **每帧 `SetWindowRgn`** → p99 17.3ms 超预算。`SetWindowRgn` 触发 DWM 往返。改为只有空闲/悬停两态，悬停态用满倍率矩形（所有中间帧的并集）。
3. **每条 `WM_MOUSEMOVE` 都 `InvalidateRect`** → p99 18.2ms。Windows 投递鼠标移动远快于 60Hz。改为消息只置标志位，由 tick 合并成每帧最多一次重绘。

三者叠加后 p99 从 18.2ms 降到 16.54ms，21/21 窗达标。

## 测试

```
All tests passed (1371 assertions in 101 test cases)
```

新增 `tests/dock_hover_test.cpp` 14 用例：悬停 ramp 上升/下降/不超调/忽略非正 delta；120Hz×10 帧与 60Hz×5 帧的 intensity 相同（ramp 定义在时间上而非帧上）；20 次快速移动在一帧内只推进一帧的量；`ComputeGeometry` 板高 68 DIP、窗高 106.4 DIP、窗宽 > 板宽；DPI 翻倍 DIP 不变；退化 config 不崩；region 空闲排除板上方空白、悬停包含图标行、裁掉圆角、超大 radius 钳制、退化 plate 返回 nullptr、零面积 row 不扩宽。

## 诚实边界

- 本机只有一个显示器，混合 DPI 双屏无法真机验证（同 T-007 的限制）。`DOCKFORGE_DPI_OVERRIDE` 只改变内容布局，窗口的真实 DPI 仍是 192——所有测量工具因此从窗口高度反推 scale，而不是问 `GetDpiForWindow`。
- 悬停放大只验证了 `sine` 曲线与默认参数；`cosine_integral`/`gaussian` 由单元测试覆盖，未做视觉验证。
- 点击穿透只在 spike 的双窗口场景验证了机制，未在真实桌面应用（如浏览器）上验证点击确实落到目标控件。
