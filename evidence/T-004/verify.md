# T-004 · platform: DirectComposition 分层窗口 spike

状态：**DONE**
日期：2026-10-23
提交：见 `git log`（`[T-004] platform: DirectComposition surface + spike`）

## DoD 核对

| # | 验收条件 | 结果 | 证据 |
|---|---|---|---|
| 1 | spike 独立可编译，创建 `WS_EX_NOREDIRECTIONBITMAP` + `WS_EX_LAYERED` 窗口 | PASS | `spikes/t004_dcomp/`（四种扩展样式全跑通）、`run_plain.txt` / `run_noredir.txt` / `run_layered.txt` / `run_both.txt` |
| 2 | `DCompositionCreateDevice2` → `IDCompositionDesktopDevice` → `CreateTargetForHwnd` 成功（HRESULT 打印） | PASS | `run_final.txt`，全链路 `hr=0x00000000` |
| 3 | 窗口显示纯色 visual，无黑块，圆角外区域真透明 | PASS | `alpha_matrix.txt`（三档 DPI 差分采样）、`dpi96.png` / `dpi144.png` / `dpi192.png` |
| 4 | 结论写入 DECISIONS.md（成功则纳入主工程） | PASS | `DECISIONS.md` ADR-0007；`src/platform/composition.{h,cpp}` 已进主工程并被 `src/main.cpp` 使用 |

## 为什么"透明"必须做差分采样

`WS_EX_NOREDIRECTIONBITMAP` 窗口没有重定向位图，屏幕抓图里"这里是深色"完全不能证明窗口画了什么——壁纸本身可能就是深色。T-004 第一版证据就栽在这里：窗口中心采样 `rgba=(54,51,69)`，看似面板色，但同一坐标在窗口销毁后是 `rgba=(191,169,241)`（壁纸亮紫），说明**面板根本没画上去**，读到的是壁纸。

因此新增 `tools/alpha_probe.ps1`：同一屏幕区域抓两次，一次窗口在、一次进程被杀掉，逐点比较。
- 两点颜色一致（容差 2）→ `TRANSPARENT`（桌面透出来了）
- 两点颜色不同 → `PAINTED`（应用真的画了）

第一版用 `ShowWindow(SW_HIDE)` 取第二次样本，结果自相矛盾（隐藏后仍抓到窗口内容），因为隐藏经 DWM 异步生效。改成 `Stop-Process -Force` 后结果稳定。工具本身也加了 `$ErrorActionPreference = 'Stop'`：一个找不到窗口却报告"全部透明"的探针等于伪造证据。

## 采样结果（三档 DPI，全部符合预期）

`alpha_matrix.txt` 摘要，点位定义见 `tools/alpha_probe.ps1` 的 `-Auto`：
`center` 面板中心、`corner_*` 四角（圆角外）、`top_mid` 上边缘中点（发丝描边）。

| DPI | 物理尺寸 | center | corner_tl | corner_tr | corner_bl | corner_br | top_mid |
|---|---|---|---|---|---|---|---|
| 96 | 528x152 | PAINTED | TRANSPARENT | TRANSPARENT | TRANSPARENT | TRANSPARENT | PAINTED |
| 144 | 792x228 | PAINTED | TRANSPARENT | TRANSPARENT | TRANSPARENT | TRANSPARENT | PAINTED |
| 192 | 1056x304 | PAINTED | TRANSPARENT | TRANSPARENT | TRANSPARENT | TRANSPARENT | PAINTED |

每档都是 `painted=2 transparent=4`，与"圆角矩形 + 1px 描边"的几何一致：中心与上边缘被画，四角在圆弧外保持透明。

## 两个必须记住的 DComp 陷阱（已写入 ADR-0007 与 STATE.json）

**1. `BeginDraw` 返回的 offset 是物理像素，`SetTransform` 吃的是 DIP。**
`IDCompositionSurface::BeginDraw(..., &offset)` 给出的是本 surface 在合成图集里的偏移，单位物理像素；而 D2D 的 transform 已经按 `SetDpi` 归一化到 DIP。直接把 `offset` 塞进 `Matrix3x2F::Translation` 会在非 96 DPI 上把内容推偏 `(dpi/96 - 1) * offset`。

实测症状：144 DPI 下窗口中心采样为 `TRANSPARENT`（面板整体偏移出可视区），96/192 DPI 侥幸正常。修复：

```cpp
const float dip_scale = 96.0f / (dpi > 0.0f ? dpi : 96.0f);
dc->SetTransform(D2D1::Matrix3x2F::Translation(
    offset.x * dip_scale, offset.y * dip_scale));
```

**2. `ID2D1DeviceContext::GetPixelSize()` 返回图集尺寸，不是 surface 尺寸。**
拿它算布局会把圆角矩形画到窗口外。96 DPI 下 `corner_tr` 被采成 `PAINTED` 就是这个 bug（矩形右边超出窗口）。修复：`CompositionSurface::Paint` 改为向回调传 `PaintInfo{dpi, width_dip, height_dip}`，DIP 尺寸由 `width_px_ * 96 / dpi` 算出，布局一律以它为准。

## 主工程接线

`src/main.cpp` 从 GDI `WM_PAINT` 改为 DComp：

- `WindowDesc.composition_surface = true` → `WS_EX_NOREDIRECTIONBITMAP`
- `CompositionSurface surface; surface.Initialize(window.Handle())`
- `WM_PAINT` 里 `BeginPaint`/`EndPaint` 仅用于消费更新区，实际绘制走 `surface.Paint(dpi, cb)` + `surface.Commit()`
- `WM_SIZE` / `WM_DPICHANGED` 里 `surface.Resize(w, h)` 后重绘
- 内容：D2D 圆角矩形（`config.appearance.corner_radius`）+ 1px 半透明描边 + DirectWrite 标签（`Segoe UI Variable Display`，缺失时回退 `Segoe UI`）

## DPI 覆盖手段

本机是 192 DPI 单屏，无法靠改系统设置覆盖 96/144 两档。加测试钩子 `DOCKFORGE_DPI_OVERRIDE=<96|120|144|192>`：`platform::Window::SetDpiOverride(UINT)` 让 `Dpi()` 返回指定值，从而驱动窗口物理尺寸与 D2D `SetDpi` 走完整缩放路径。仅测试用，非用户配置项。

## 性能与资源（`idle.txt` / `handle_soak.txt`）

| 指标 | 值 | 预算 | 状态 |
|---|---|---|---|
| 空闲 CPU（60s 平均，18 核） | 0.0043% | < 0.5% | PASS |
| 空闲 RSS | 65.80MB → 65.85MB | < 120MB | PASS |
| 句柄（60s） | 249 → 250 | 无增长 | PASS |
| 线程数 | 5 | — | 新增 D3D11/DXGI/D2D/DComp 内部线程，符合预期 |

RSS 从 T-003 的 11.5MB 涨到 65.8MB，是 D3D11 + D2D + DComp 设备栈的固定开销（驱动侧显存映射计入工作集）。仍在 120MB 预算内，但后续加缩略图/壁纸模糊时必须盯住这条曲线。

## 构建与测试

- `build.log`：`dockforge_lib` / `DockForge` / `dockforge_tests` 三个目标全部 Built
- `test.txt`：`All tests passed (137 assertions in 27 test cases)`

## 遗留

- 主工程尚未做材质（Mica/Acrylic/模糊），当前是纯 D2D 半透明填充。归 T-005。
- `WM_PAINT` 每帧重建 brush，动画任务接入后需改为缓存 brush（记入 T-010 上下文）。
