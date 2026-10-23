# T-013 验收证据：Dock 内部拖拽重排

## DoD 逐条核对

| # | DoD | 结果 | 证据 |
|---|---|---|---|
| 1 | 按住拖动图标，自绘 ghost + 插入线 | PASS | `drag_visual_{96,144,192}.txt` 全部 `ghost_verdict=DRAWN_AT_POINTER` + `line_verdict=DRAWN` + `verdict=BOTH_DRAWN`；截图 `drag_mid_{96,144,192}.png` 与控制帧 `drag_hover_{96,144,192}.png` |
| 2 | 释放后顺序持久化到配置 | PASS | `drag_reorder.txt`：`reordered tile[0] -> [4]`，`config_tiles=12`，`config_tile_0` 已变为原 tile 1 |
| 3 | 拖出 Dock 区域外释放 = 移除（带确认） | PASS | `drag_eject.txt` `verdict=REMOVED` + `dock.hidden` 含该身份；`drag_eject_abort.txt` `verdict=ABORTED`（提前释放，配置 12/0 不变） |
| 4 | 拖拽期间帧率不低于 55 FPS | PASS | `drag_bench.txt`：`fps_min=62.5`、`dod_fps_verdict=PASS`、`p99_over_budget_windows=0`、`verdict=PASS` |

## spike 结论（`spikes/t013_drag/`，四组对照）

| 组 | 命令 | moves_inside | moves_outside | moves_captured_outside | leaves | track_arms |
|---|---|---|---|---|---|---|
| A | `--mode=capture --auto-drag` | 2 | 3 | **3** | 0 | 1 |
| B | `--mode=capture --auto-drag --region` | 2 | 3 | **3** | 0 | 1 |
| C | `--mode=capture --auto-drag --no-capture` | 2 | **0** | 0 | 0 | 1 |
| D | `--mode=leave` | 1 | 0 | 0 | **1** | 1 |

1. **捕获后指针离开窗口仍收到 `WM_MOUSEMOVE`**：A 组 `moves_captured_outside=3`，不捕获的 C 组 `moves_outside=0`。
2. **region 不影响消息投递**（A/B 逐项相同），但仍裁剪合成内容（C-12 结论 3），所以拖拽期间 region 必须扩到整窗——这是绘制问题不是消息问题。
3. **拖拽期间不收到 `WM_MOUSELEAVE`，归因是按键按下不是捕获**：A/B/C 全为 0（都按了键），D 组（不按键）为 1。

**方法论教训**：第一版用 C 组当 leave 对照，得出"捕获抑制 leave"；但 C 组也按了键，无法区分"捕获抑制"和"按键抑制"。真正的对照是 D 组。且 `TrackMouseEvent` 的返回值最初被忽略——控制组报 `leaves=0` 时唯一诚实的读法是"这个 spike 根本看不见 leave"。**没有对照组的「零」不是证据。**

## 实现要点

- `src/dock/drag_controller.{h,cpp}`：`DragPhase{Idle,Pending,Dragging,Ejecting}`，阈值 4 DIP 从按下点测，`kDragEjectDip=48`，`kEjectConfirmMs=550`。
- `src/dock/tile_order.{h,cpp}`：`IdentityOf`（AUMID → namespace → path → name，LowerAscii）、`ApplyOrder`（空=非托管自动填充，非空=托管不补位）、`MoveTile`（erase+insert 不是 swap）、`RemoveTile`、`CommitOrder`（立即写盘）。
- `src/dock/dock_view.{h,cpp}`：`IdleCentersDip`（静止栅格）、ghost 淡出 `kDragGhostOpacity * (1 - 0.65*progress)`、插入线 `kDropLineWidthDip=3` / `kDropLineHeightFactor=0.62`。
- `src/core/config.{h,cpp}`：`dock.tiles` / `dock.hidden`，≤256 项 / ≤512 字符，坏条目丢弃并置 `needs_rewrite`（不整份回退）。
- `src/core/paths.cpp`：`ConfigPath()` 支持 `DOCKFORGE_CONFIG_PATH` 覆盖。
- `src/main.cpp`：`WM_LBUTTONDOWN` 只 `drag.Begin` + `SetCapture` + `apply_region()`；`WM_LBUTTONUP` 按 `Kind` 分派；`WM_CANCELMODE` → `Cancel`；tick 调 `drag.Tick(delta_ms)`。

## 五处缺陷与修正（每条都有实测）

| # | 症状 | 根因 | 修正 |
|---|---|---|---|
| 1 | `0 -> 4` 的拖拽报成 `0 -> 3` | `InsertionIndexFor` 吃放大后的槽中心，放大是鼠标位置的函数，形成反馈环 | 新增 `IdleCentersDip`（解析式静止栅格），`drag.Move` 改用它 |
| 2 | 目标永远差一槽 | 比较用 `>`，指针落在槽中心时不算"已越过" | 改为 `>=` |
| 3 | 测试污染开发机真实配置 | `CommitOrder` 用例直接写 `%APPDATA%\DockForge\config.json`，留下 `tiles:["alpha.app"]` | `ConfigPath()` 支持 `DOCKFORGE_CONFIG_PATH`；`tests/main_test.cpp` 静态初始化期隔离到 `%TEMP%` |
| 4 | 探针报 `capture_taken=False` | `GetCapture()` 返回**调用线程**的捕获窗口，跨进程恒 0 | 删掉探针断言，改读 app 自报 `drag press ... capture=1 match=1` |
| 5 | 一进 eject 区释放就删 tile | 无确认 | `DragController::Tick` 累加 `eject_hold_ms`，550ms 才 `Remove`，否则 `Cancel`；ghost 随进度淡出 |

## 拖拽视觉验证（`tools/drag_visual.ps1`）

| DPI | ghost bbox | near_pointer | 插入线 | verdict |
|---|---|---|---|---|
| 96 | 319x28 | 1056（阈值 173） | x=464 width=2 peak=29 | BOTH_DRAWN |
| 144 | 478x42 | 2374（阈值 389） | x=695 width=4 peak=43 | BOTH_DRAWN |
| 192 | 639x57 | 4318（阈值 691） | x=927 width=5 peak=58 | BOTH_DRAWN |

**控制帧必须是按下点，不是释放点**：拖拽期间 `WM_MOUSEMOVE` 提前 return，`hover.mouse_x_dip` 不更新，行放大围绕按下点保持。停在释放点的控制帧会放大另一个图标，差分结果全是图标横移，证明不了 ghost（实测该错误配置下 ghost bbox 宽度 883px，narrow_run 6 个，无法判读）。

## 性能

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| 拖拽 fps_min（15s，14 窗） | **62.5** | ≥55 | PASS |
| 拖拽 p99 窗口超预算数 | **0** | 0 | PASS |
| 拖拽 p99_mean / p99_max | 16.578 / 16.69ms | <16.7ms | PASS |
| 空闲 CPU（60s，18 核） | 0.156s / **0.0144%** | <0.5% | PASS |
| 空闲 RSS | 90.59 → 90.57MB | <120MB | PASS |
| 句柄 / 线程 | 372→362 / 10→6 | 无增长 | PASS |

**首窗 p99 曾报 19.63ms / OVER_BUDGET 的归因**：`core::FrameClock` 是 1000 样本滚动窗，启动期样本（首帧 309ms 的模糊 shader 编译）要在 1000 个新样本后才滚出。`drag_bench.ps1` 改为先等日志出现 `frames=1000` 再取基线后，首窗降到 16.71ms 且稳态每窗 16.52–16.58ms。

## 测试

`All tests passed (1578 assertions in 174 test cases)`（T-012 时 1458/125）。

新增 `tests/dock_drag_test.cpp` 21 用例（`[dock][drag]`）+ `tests/tile_order_test.cpp` 23 用例（`[dock][order]`）+ `tests/dock_hover_test.cpp` 新增 1 用例 `"idle slot centres do not move when the row magnifies"`（反向断言 `IconCentersDip` 至少有 0.5 差异，否则测试在"放大从未运行"的构建上也会通过）。

配置隔离验证：跑完测试后真实 `config.json` 仍是 `tiles=12 hidden=0`。

## 诚实边界

1. **单显示器主机无法验证跨屏拖拽**。本机只有一块 2880x1920，拖到副屏未测。
2. **拖拽视觉验证依赖人工传入几何参数**（`-PlateTop` / `-LineTop` / `-LineBottom` / `-PointerX` / `-PointerY` / `-GhostHalfPx`）。这些值由探针输出的 `scale`、`origin` 和 painter 常量推导，工具不做独立校验——传错会得到自信的错误答案。
3. **插入线用"≤10px 宽的亮列"识别**，图标边缘的窄高光也可能命中。当前三档实测每档只有 1 个 narrow_run，但图标集变化后需要复核。
4. **`-EjectAbort` 的判据是"没有任何日志行"**。如果 `WM_LBUTTONUP` 根本没送达，也会报 `ABORTED`。当前 `drag_eject.txt`（同样路径、同样注入、只是多等 900ms）报 `REMOVED`，证明送达链路是通的，但这是间接证据。
