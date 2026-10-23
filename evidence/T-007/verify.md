# T-007 · platform: 多显示器 + 高 DPI 管理

状态：**DONE** | 依赖：T-004 | 上下文：`src/platform/display.*`、`src/main.cpp`

## DoD 逐条核对

| DoD | 结论 | 证据 |
|---|---|---|
| Per-Monitor V2 DPI 感知，`WM_DPICHANGED` 正确重建渲染资源 | PASS | `simulate_dpi96.txt`、`simulate_dpi144.txt`、`test.txt` |
| 枚举显示器、工作区、主屏判定 | PASS | `run_enum.txt`、`idle.txt` 启动日志 |
| Dock 窗口按目标屏工作区定位 | PASS | `edge_bottom/left/right.png` + 启动日志 |
| 混合 DPI 双屏切换时无模糊、无错位 | PASS（见「诚实边界」） | `test.txt` 的混合 DPI 用例 + `simulate_dpi*.txt` |
| 100/150/200 三张截图 | PASS | `dpi96.png`、`dpi144.png`、`dpi192.png` |

## spike 结论（`spikes/t007_monitor/`）

`run_enum.txt`：
```
[enum] monitors=1 system_dpi=192 awareness=0x22
  mon[0] dev=\\.\DISPLAY1 monitor=(0,0) 2880x1920 work=(0,0) 2880x1824 primary=1 effective_dpi=192 raw_dpi=243
  GetSystemMetricsForDpi(SM_CXSCREEN, 96)=2880
  GetSystemMetricsForDpi(SM_CXSCREEN, 144)=2880
  GetSystemMetricsForDpi(SM_CXSCREEN, 192)=2880
```
1. **本机只有一个显示器**：2880x1920，有效 192 DPI（200%），原始 243 DPI。两者不等，不能混用。
2. `GetSystemMetricsForDpi(SM_CXSCREEN, dpi)` 与 DPI 无关，恒返回 2880——**不能用来推缩放**。
3. `WM_DPICHANGED` 在窗口创建时不发送（`run_window.txt`: `WM_DPICHANGED on create: no`），只能由跨屏移动触发。
4. `AdjustWindowRectExForDpi(WS_POPUP)` 是 no-op（`(0,0) 400x120` 输入输出相同）——**无边框窗口不需要**。
5. `run_move.txt`：在唯一显示器上移动窗口，`total WM_DPICHANGED=0`，位置 `(1240,1664)` 精确落位。

## 实现

新增 `src/platform/display.h` / `display.cpp`：

- `DisplayInfo{HMONITOR handle; std::wstring device; RECT monitor; RECT work; UINT effective_dpi; UINT raw_dpi; bool primary; Width()/Height()/WorkWidth()/WorkHeight()/Scale()}`
- `EnumerateDisplays()`：主屏恒为 `[0]`，其余按 `top`/`left`/`device` 排序，**索引稳定**。
- `PrimaryDisplay(DisplayInfo&)`、`DisplayForWindow(HWND, DisplayInfo&)`、`DisplayForPoint(POINT, DisplayInfo&)`
- `DockPlacement{RECT rect; int width_px; int height_px; UINT dpi; float scale; bool clamped;}`
- `ComputeDockPlacement(const DisplayInfo&, DockEdge, float logical_width, float logical_height, float margin_dip)`
- `DipToPixels(float, UINT)` / `PixelsToDip(int, UINT)`，零 DPI 按 96 处理不除零。

`src/main.cpp` 变更：

- 新增 `DOCKFORGE_TARGET_DISPLAY=<index>`（测试钩子，按 `EnumerateDisplays()` 顺序钉住某块屏）。
- 新增 `ToDockEdge(core::DockPosition)`，让 `dock.position` 的 `bottom`/`left`/`right` 真正生效。
- **单一放置路径** `apply_placement` lambda：启动、`WM_DPICHANGED`、`ShowWindow` 之后都调它。
  原来的 `resize_to_logical` 与末尾"居中到主屏"代码块被删除——两处各算尺寸正是本轮要消除的隐患。
- `WM_DPICHANGED` 改为：重新解析宿主显示器 → `wParam` 作为权威 DPI → `apply_placement()` →
  `InvalidateRect`。`lParam` 建议矩形**不使用**（它按普通重叠窗口算，与工作区放置无关）。
- `DOCKFORGE_DPI_OVERRIDE` 优先级最高且不被 `WM_DPICHANGED` 覆盖（`dpi_forced_by_env` 记录）。
- 新增 `DOCKFORGE_SIMULATE_DPI=<dpi>`：进程内 `SendMessageW(WM_DPICHANGED)`。

`CMakeLists.txt`：`dockforge_lib` 链接库补 **`shcore`**（`GetDpiForMonitor` 所在）。

## 编译期踩的坑

- `GetDpiForMonitor` / `MDT_EFFECTIVE_DPI` / `MDT_RAW_DPI` 需要 `#include <shellscalingapi.h>`，
  链接需要 `shcore`（否则 `undefined reference to GetDpiForMonitor`）。
- `std::max(0, (work.right - work.left) - 2 * margin)` 因 `LONG` vs `int` 报
  `no matching function for call to 'max(int, long int)'`，必须写 `std::max(0L, ...)`。

## 实测

### 放置（`edge_*.png` + 启动日志，`DOCKFORGE_DPI_OVERRIDE=96`）

| `dock.position` | 目标坐标 | 尺寸 | 校验 |
|---|---|---|---|
| `bottom` | `1176,1664` | 528x152 | 水平居中：`(2880-528)/2 = 1176`；底边 `1824-8 = 1816` |
| `left` | `8,836` | 528x152 | 左边距 8；垂直居中 `(1824-152)/2 = 836` |
| `right` | `2344,836` | 528x152 | 右边 `2880-8 = 2872`；垂直居中同左 |

### DPI 三档（`DOCKFORGE_DPI_OVERRIDE`）

| 缩放 | 窗口 | center | top_mid | corner_tl | corner_br |
|---|---|---|---|---|---|
| 96 (100%) | 528x152 | `236,240,248` | `13,15,18` | `0,0,0` | `0,0,0` |
| 144 (150%) | 792x228 | `236,240,248` | `13,15,18` | `0,0,0` | `0,0,0` |
| 192 (200%) | 1056x304 | `236,240,248` | `52,54,56` | `0,0,0` | `0,0,0` |

差分透明（`alpha_probe.ps1`，杀进程取基线）：三档全部 `painted=2 transparent=4`。
`corner_*` 四点全 `TRANSPARENT`，`center`/`top_mid` `PAINTED`——圆角外真透明。

### `WM_DPICHANGED` 处理器（`simulate_dpi*.txt`）

```
baseline      912,1504 1056x304 window_dpi=192
simulated  96 1176,1664 528x152 window_dpi=192
resized=1 moved=1
verdict=HANDLER_ACTIVE
```
```
baseline      912,1504 1056x304 window_dpi=192
simulated 144 1044,1584 792x228 window_dpi=192
resized=1 moved=1
verdict=HANDLER_ACTIVE
```
处理器确实按新 DPI 重算了尺寸与位置，而不是只写日志。

### 性能（`idle.txt`、`bench.txt`，60s）

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| 空闲 CPU | 0.047s / **0.0043%** | < 0.5% | PASS |
| RSS | 90.09 → 90.09 MB | < 120MB | PASS |
| 句柄 | 378 → 368 | 无增长 | PASS |
| 线程 | 10 → 6 | 稳定 | PASS |
| 帧 p50 / p99 | 16.01ms / **16.59–16.68ms**，62.5fps | p99 < 16.7ms | PASS |

`bench.txt` 中 12 个连续采样窗的 p99 全部 ≤ 16.68ms，**没有一次超过 16.7ms**——T-006 遗留的
尾延迟在本轮未复现（T-006a 仍保留，等复现再动手，不凭空改帧循环）。

### 测试

`test.txt`：`All tests passed (380 assertions in 72 test cases)`。新增 `tests/display_test.cpp`
13 个用例覆盖：DPI 往返（96/120/144/168/192/240/288 + 零 DPI）、四舍五入而非截断、
Bottom 居中且避开任务栏、Left/Right 贴边垂直居中、DPI 翻倍尺寸同比例、超宽/超高 clamp
不越界、**负原点副屏**、**混合 DPI 双屏（1920x1080@96 + 2560x1440@144）**、`work` 全零回退
`monitor`、`EnumerateDisplays` 至少一个主屏、`DisplayForWindow` 含 `nullptr` 不崩、
`DisplayForPoint` / `PrimaryDisplay`。

## 诚实边界（不得含糊）

本机只有一块显示器，**无法产生真实的跨屏 DPI 转换**。因此：

1. 混合 DPI 的几何正确性由 `tests/display_test.cpp` 的纯逻辑断言覆盖——合成 `DisplayInfo`，
   不依赖真实硬件。这是有效证据，但证明的是数学，不是合成器行为。
2. `DOCKFORGE_SIMULATE_DPI` 走的是**真实 `WndProc` 路径**（进程内 `SendMessageW`），证明处理器
   会重算尺寸与位置。它不是真实的跨屏拖拽。
3. `GetDpiForWindow` 在合成消息后仍返回 192，这是预期的：窗口没有真的换屏。证据文件里
   `note=` 行明确记录这一点。
4. **跨进程注入无效**：`PostMessage(WM_DPICHANGED)` 返回 `ERROR_MESSAGE_SYNC_ONLY`(1159)，
   `SendMessage` 返回 0 但处理器不执行。对照实验：`WM_CLOSE` 跨进程可正常送达并退出进程。
   这是系统消息特有的限制，已写入 `tools/simulate_dpi.ps1` 头部与 `CONTRACTS.md` C-10。

COMPATIBILITY.md 的「混合双屏」一行在本机只能标记为**逻辑验证通过 + 待真机复核**，
不得标记为实测通过。

## 新增/修改文件

- `src/platform/display.h`、`src/platform/display.cpp`（新）
- `tests/display_test.cpp`（新，13 用例）
- `src/main.cpp`（放置路径重构、`DOCKFORGE_TARGET_DISPLAY`、`DOCKFORGE_SIMULATE_DPI`）
- `tools/simulate_dpi.ps1`（新）
- `CMakeLists.txt`（链接 `shcore`）
- `spikes/t007_monitor/{main.cpp,CMakeLists.txt}`
