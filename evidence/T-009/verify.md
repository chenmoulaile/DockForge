# T-009 · dock: Dock 窗口渲染（静态图标行）

## 结论

PASS。Dock 窗口在 DPI 96 / 144 / 192 三档均渲染出 12 个真实应用图标，逐槽位像素校验 `ALL_SLOTS_DRAWN`，空闲 RSS 77.83MB、CPU 0.0029%、句柄无增长。

## DoD 核对

| DoD | 结果 | 证据 |
|---|---|---|
| 底部居中渲染 N 个图标，材质底板 + 圆角 | PASS | `dpi{96,144,192}.png`、`slots{96,144,192}.txt` |
| 图标按 C-05 基础位置排布，间距/尺寸读配置 | PASS | `placed.txt`（配置 icon=48 gap=8 mag=1.8）、`tests/layout_test.cpp` 15 用例 |
| 窗口自动贴合内容宽度 | PASS | 窗口 = 内容 + 2×`kPlatePaddingDip`，见下表 |
| 三张 DPI 截图 | PASS | `dpi96.png` 684x68、`dpi144.png` 1026x102、`dpi192.png` 1368x136 |
| 空闲 RSS < 120MB | PASS | `idle.txt` 77.83MB |

## 窗口几何（C-01 默认配置 icon_size=48 gap=8，12 个图标）

内容宽 = 12×48 + 11×8 = 664 DIP；板宽 = 664 + 2×10 = 684 DIP；板高 = 48 + 2×10 = 68 DIP。

| DPI | 物理窗口 | 期望 = round(DIP×dpi/96) | 位置（底部居中，边距 8 DIP） |
|---|---|---|---|
| 96 | 684x68 | 684x68 | `1098,1748`，底边 1816 = 1824−8 |
| 144 | 1026x102 | 1026x102 | `927,1710`，底边 1812 |
| 192 | 1368x136 | 1368x136 | `756,1672`，底边 1808 |

三档水平居中均满足 `(2880 − 宽) / 2`。DPI 从 96 到 192 尺寸精确翻倍，无累积舍入误差。

## 逐槽位像素校验

`tools/dock_slots.ps1` 对每个槽位 `[cx−Half, cx+Half] × [cy−Half, cy+Half]` 以 2px 步长采样，与板色 `13,15,18`（容差 6）比较，统计非板色像素占比与包围盒。

- 96 DPI：`FirstCenter=34 Stride=56 Half=26` → `slots=12 with_icon=12 total_drawn_samples=2515`、`ALL_SLOTS_DRAWN`
- 144 DPI：`FirstCenter=51 Stride=84 Half=40` → `slots=12 with_icon=12 total_drawn_samples=5414`、`ALL_SLOTS_DRAWN`
- 192 DPI：`FirstCenter=68 Stride=112 Half=44` → `slots=12 with_icon=12 total_drawn_samples=8375`、`ALL_SLOTS_DRAWN`

各槽 coverage 6.6%–81.9%，差异来自图标自身的透明区域面积，不是位置偏移：所有包围盒都落在槽位内（如 192 DPI 槽 0 `52,56-84,80`、槽 11 `1256,32-1344,102`，槽宽 112 中心 1300）。

**为什么不用单点采样**：深色中心的图标（如 7-Zip 的黑色块）单点会读成板色，被误判为缺失。槽位覆盖率法能区分「图标很暗」和「槽位是空的」。

## tile 来源

`dock tiles: 12 of 215 apps (limit=12 icon=256px)`，按 AppsFolder 枚举顺序取前 12：

| # | 名称 | AppKind | 图标 |
|---|---|---|---|
| 0 | 7-Zip | executable | ok |
| 1 | Ahk2Exe | shell-item | ok |
| 2 | AI 空间 | namespace-item | ok |
| 3 | AutoClaw | shell-item | ok |
| 4 | AutoHotkey | executable | ok |
| 5 | AutoHotkey Dash | shell-item | ok |
| 6 | AutoHotkey Help File | executable | ok |
| 7 | AutoHotkey Setup | executable | ok |
| 8 | AutoHotkey Window Spy | shell-item | ok |
| 9 | BandiView | namespace-item | ok |
| 10 | Bandizip | namespace-item | ok |
| 11 | Beautiful | executable | ok |

三种身份（可执行文件路径 / 裸桌面应用 id / 命名空间子项）在同一行里都成功取到图标，验证了 C-08 的 `IconSourceFor` 分派在真实数据上成立。

## 性能

`evidence/T-009/idle.txt`，60 秒，18 核：

```
cpu_total_s=0.031 cpu_percent=0.0029
rss_mb=89.68 -> 77.83
handles=372 -> 364
threads=10 -> 6
```

- CPU 0.0029%（预算 <0.5%）
- RSS 77.83MB（预算 <120MB），较 T-007 的 90.09MB 下降，因为窗口从 1056x304 缩到 1368x136 之外的调试尺寸
- 句柄 372→364、线程 10→6，无增长

帧时间 12 个连续采样窗：`avg=16.00ms p50=16.01ms p99=16.59–16.64ms fps=62.5`，全部低于 16.7ms 预算。

## 本轮修复的缺陷

`dockforge::shell::ToString(AppKind)` 在 `src/shell/app_entry.h:18` 只有声明没有定义（T-006 遗留）。任何调用它的翻译单元都链接失败，加逐 tile 诊断日志时暴露为 `undefined reference to dockforge::shell::ToString(dockforge::shell::AppKind)`。已在 `src/shell/app_resolver.cpp` 补齐六值映射。

## 实现要点

- `src/render/backend.h` 新增 `BitmapView{width,height,pixels,Valid()}` 与 `DrawBitmap(cache_id, view, rect, opacity)`。`cache_id` 是内容身份而非像素地址——按指针做键会在 vector 重分配后每帧重传位图，按视图地址会别名。
- `src/render/backend_d2d.cpp` 用 `std::unordered_map<std::uint64_t, BitmapEntry>` 缓存 `ID2D1Bitmap1`，尺寸变化才重建。位图 DPI 固定写 96：目标矩形已是 DIP，backend 变换已带帧 DPI，若再打帧 DPI 会二次缩放。
- `src/dock/dock_view.{h,cpp}` 承载 tile 构建、几何计算与绘制。先画板再画图标——霜层盖住图标就白做了。
- 缺失图标画淡色圆环占位而不是留空：缺图是外观问题，缺启动器是功能问题，两者不能长得一样。
- `src/main.cpp` 删除了 `LogicalSize`/`ComputeLogicalSize` 这组与 `apply_placement` 重复计算尺寸的代码，几何只由 `dock::ComputeGeometry` 一处产生，避免两处漂移。

## 诚实边界

- 悬停放大、运行指示、拖拽、缩略图均未实现（T-010 起）。本任务的"窗口贴合内容宽度"指基础尺寸，鼠标交互时窗口扩张属 T-010。
- 三档 DPI 通过 `DOCKFORGE_DPI_OVERRIDE` 强制，非真实跨屏转换；真实混合 DPI 见 `COMPATIBILITY.md` 的"逻辑通过（不可真机测）"。
- 图标取自本机 AppsFolder 前 12 项，测试机特定；排序/筛选策略属 T-011。
