# T-014 验证：dock 接受外部 OLE 拖入

## DoD 核对

| DoD | 结果 | 证据 |
|---|---|---|
| 从资源管理器拖 `.lnk` 到 dock，松手后新 tile 出现在落点位置 | 通过 | `drop_cases_dpi192.txt`：`drop: added ...cursor.exe at tile[6] (13 tiles)`，落点 `insert=6` |
| 拖 `.exe` / 目录同样可加入 | 通过 | 同文件 `case=folder`：`added ...dockforge-drop-folder at tile[7] (14 tiles)`；`.exe` 走同一 `DropKind::Executable` 分支（单测覆盖） |
| 拖文档类不可接受，caret 变红且松手无变化 | 通过 | `case=document`：`kind=unsupported acceptable=0` → `drop leave` → `after_document tiles=13`；红色 caret 峰值 `(219,98,89)` vs 接受色 `(212,218,227)` |
| 重复拖同一个应用不产生第二个 tile | 通过 | `case=shortcut_again`：`is already on the dock at tile[6]`，`after_again tiles=13` |
| 拖入期间行回落、caret 落在静止栅格 | 通过 | `drop_caret_probe_*`：96/144/192 三档 caret 中心与 `ExpectedX` 差 0/0/1px，均在 14px 容差内 |
| 拖入期间帧预算 p99 < 16.7ms | 通过 | `drop_bench.txt`：`p99_mean=16.512ms p99_max=16.540ms`，`p99_over_budget_windows=0`，`fps_min=62.5` |
| 空闲成本不回归 | 通过 | `idle.txt`：CPU `0.0173%`、RSS `94.16→94.14MB`、句柄 `394→386`、线程 `10→6` |
| 单测覆盖分类与槽位边界 | 通过 | `All tests passed (1634 assertions in 183 test cases)`，新增 `tests/dock_drop_test.cpp` 10 用例 |

## spike 四条硬约束（`spikes/t014_drop/`）

`main.cpp` 自建 `FileDataObject`（只实现 `CF_HDROP`）、`MouseDropSource`、`LoggingDropTarget`，`DriverThread` 用 `SendInput` + `SetCursorPos` 自驱完整握手，可与主工程分两个进程跑。

### 1. `DoDragDrop` 必须传非空 `IDropSource`

传 `nullptr` → `hr=0x80070057 E_INVALIDARG`，一个回调都不触发。补 `MouseDropSource`（`QueryContinueDrag` 里 escape → `DRAGDROP_S_CANCEL`、松开左键 → `DRAGDROP_S_DROP`）后全链路通。

### 2. `RegisterDragDrop` 必须在 `OleInitialize` 之后

同一进程同一窗口，只换 COM 初始化方式：

| 初始化 | `RegisterDragDrop` |
|---|---|
| `OleInitialize(nullptr)` | **`S_OK`** |
| `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` | **`0x8007000E E_OUTOFMEMORY`** |
| `CoInitializeEx(..., COINIT_APARTMENTTHREADED \| COINIT_DISABLE_OLE1DDE)` | **`0x8007000E`** |

`E_OUTOFMEMORY` 是误导性错误码，真实含义是"OLE 没初始化"。`src/main.cpp` 原本写的是 `CoInitializeEx(..., COINIT_DISABLE_OLE1DDE)`，已换成 `dock::OleSession`。

### 3. region 裁剪会吞掉 region 外的 `DragEnter`/`Drop`

窗口 800x200 @ `600,1500`，region 从 client y=80 起，逐百分比拖入：

| drop_y | 结果 |
|---|---|
| 38% | `NO_ENTER` |
| 39% | `NO_ENTER` |
| **40%** | `DragEnter client=(400,80)` + `Drop` + `hdrop_count=1` |
| 41% | `DragEnter client=(400,82)` + `Drop` + `hdrop_count=1` |

边界严格等于 region 顶边。无 region 时 20% 也能进。

### 4. 进入后必须扩 region，否则 `Drop` 永不触发

两段式路径（先进 region 内 client y=120，再上移到 y=20）：

- 扩：`DragEnter` → `SetWindowRgn(full)` → `DragOver #20 client=(400,20)` → `Drop` + `hdrop_count=1`，`DoDragDrop hr=0x00040100`（`DRAGDROP_S_DROP`）。
- 不扩：`DragEnter` → `DragLeave over_count=6` → 日志停在 `QueryContinueDrag -> DROP`，进程挂住，须 `Stop-Process -Force`。

### 跨进程握手

```
target: DragEnter screen=(1000,1620) client=(400,120) acceptable=1
  expand_on_enter: SetWindowRgn(full 800x200) -> 1
Drop screen=(1000,1620) client=(400,120) over_count=5
  hdrop_count=1 path[0] attrs=0x00000020 ...\Start Menu\Programs\Cursor.lnk
source: DoDragDrop hr=0x00040100 effect=0x00000001
```

## 实现要点

`src/dock/drop_target.{h,cpp}`：`OleSession`（引用计数 RAII）、`DropPointer`、`DropSink`（三个 `std::function`）、`DockDropTarget : IDropTarget`。用 `std::function` 而非纯虚基类，因为实现全在 `main.cpp` 的 lambda 里，纯虚会逼着把 dock 状态搬进类。

`src/shell/drop_resolver.{h,cpp}`：`DropKind` 五值、`IsDockable`、`ClassifyDroppedPath`（属性优先）、`ClassifyDroppedPaths`（混合载荷整体拒绝）、`ResolveDroppedPath`（目录 → `AppKind::Folder`，其余复用 `ResolvePath`）。

`AppKind::Folder` + `LaunchFolder`：`ShellExecuteExW` 走 shell 默认动词。`CreateProcess` 对目录返回 `ACCESS_DENIED`。

`src/dock/dock_view.cpp`：`IdleCentersDip` 提为自由函数（drop target 需要在首次 paint 前就有栅格）；caret 按静止栅格解析式计算，不建 vector。

## 两处缺陷（都有数字）

### 落点错 3 槽：`GetDpiForWindow` 与几何不同源

`UpdatePointer` 原读 `GetDpiForWindow(hwnd)`（真实 192 DPI），而行几何按 `DOCKFORGE_DPI_OVERRIDE=96` 排布：

```
drop pointer screen=(1439,1789) client=(377,79) dpi=192 scale=2.000 x=188.5dip slots=12 insert=3
drop: added ... at tile[6]
```

落点算成 tile[3]，真实落进 tile[6]。加 `SetScale(window.Scale())` 后：

```
drop grid scale=1.000 slots=12 first=69.4 last=685.4 dip
drop pointer screen=(1439,1789) client=(377,79) dpi=192 scale=1.000 x=377.0dip slots=12 insert=6
```

**这是 C-10「几何与内容读同一 DPI」的第二次发作**。第一次是 T-004 的 `BeginDraw` offset（物理像素）与 `SetTransform`（DIP）混用。

### p99 17.284ms：每个 OLE 回调都 `InvalidateRect`

`DragOver` 每秒几十次，每次 `InvalidateRect` 等于 T-010 测过的消息洪流反模式：

| 配置 | p99_mean | p99_max | 超预算窗 |
|---|---|---|---|
| 每回调 `InvalidateRect` | 17.284ms | 18.490ms | 19/20 |
| tick 合并 | **16.512ms** | **16.540ms** | **0/20** |

`paint_p99_max=5.320ms`，所以超支不在绘制。

## caret 视觉验证（`tools/drop_caret.ps1`）

三档 DPI 接受色 + 192 DPI 拒绝色，全部 `DRAWN_AT_EXPECTED_SLOT`：

| DPI | 期望 x | caret x 范围 | 中心 | 宽度 | 峰值色 | 判定 |
|---|---|---|---|---|---|---|
| 96 | 381 | 379..383 | 381 | 5 | (212,217,227) | accept，delta 0 |
| 144 | 572 | 569..575 | 572 | 7 | (212,217,227) | accept，delta 0 |
| 192 | 763 | 758..766 | 762 | 9 | (212,218,227) | accept，delta 1 |
| 192 | 763 | 758..766 | 762 | 9 | **(219,98,89)** | **reject**，delta 1 |

宽度 5/7/9px = 4 DIP × 1.0/1.5/2.0 的舍入，与 `kDropCaretWidthDip` 一致。

**这个工具本身修过两次**：

1. 第一版按整帧 bbox 判读。96 DPI 得到 `0,57..754,105`（跨整行），因为行回落也在同一帧里发生。改为按列统计带内变化覆盖率，只认 **≤12px 宽的单一 run**——图标边缘 48 DIP、行回落数百像素，宽度是唯一区分器。
2. 第一版颜色取 run 的首个合格列。caret 抗锯齿，边缘列向板色混合，192 DPI 下读成 `(46,28,29)`（近黑），把 reject 判成 accept。改为取 **run 内变化最强的像素**。

## 端到端四类载荷（`drop_cases_dpi192.txt`）

```
case=shortcut        drop enter kind=shortcut acceptable=1 → added at tile[6] (13 tiles)
case=shortcut_again  is already on the dock at tile[6]     → after tiles=13
case=document        kind=unsupported acceptable=0 → drop leave → after tiles=13
case=folder          kind=folder acceptable=1 → added at tile[7] (14 tiles)
```

窗口宽度随 tile 数变化：12→1510px、13→1622px、14→1733px @ 192 DPI。

## 性能

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| 拖入 p99（20 个 1 秒窗） | 16.512ms / max 16.540ms | <16.7ms | PASS |
| 拖入 paint p99 | 5.320ms | — | 绘制不是瓶颈 |
| 拖入 fps | 62.5（min 62.5） | ≥55 | PASS |
| 空闲 CPU（60s，18 核） | 0.0173% | <0.5% | PASS |
| 空闲 RSS | 94.16 → 94.14MB | <120MB | PASS |
| 句柄（60s） | 394 → 386 | 无增长 | PASS |

## 诚实边界

1. **截图快门必须由日志驱动**。固定 `Start-Sleep` 实测在拖入前 ~200ms 开快门，得到 `changed_pixels=0 IDENTICAL`，会被读成"caret 没画"。`drop_visual.ps1` 轮询 `drop enter` 后再等 `DwellMs/2`。
2. **blur 档无法截图**。`WDA_EXCLUDEFROMCAPTURE` 让 `CopyFromScreen` 返回桌面背后内容，dock 区域全黑。视觉证据一律 `DOCKFORGE_FORCE_MATERIAL=solid`。这是快门限制，不是 caret 限制。
3. **多文件拖入无端到端截图**。`ClassifyDroppedPaths` 的整体拒绝有单测，但没做真实多选拖拽。
4. **单显示器主机**。跨屏拖入未测。
5. **spike 的双窗口叠放顺序不稳定**。两个 `WS_EX_TOPMOST` 窗口的 z 序不由创建顺序决定，`SetWindowPos(HWND_TOPMOST)` 只保证高于非 topmost 窗口。dock 只有一个 topmost 窗口，不受影响；但任何叠放两个 topmost 窗口的实验必须实测哪个在上。
