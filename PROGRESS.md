# PROGRESS.md · 执行日志（按时间正序追加）

> 每任务追加一条。格式见 PROMPT.md · CLOSE PROTOCOL。

## 2026-10-23T22:55:00+08:00 | BOOTSTRAP | DONE
- 做了什么：环境勘察（无 MSVC / 无 Windows SDK / 无 vcpkg；有 MSYS2 g++ 15.2.0 + CMake 4.4.2 + mingw32-make）；建立目录骨架；落盘 11 个状态文件；`git init` 于 `D:\MacDock`，分支 `main`，身份 `DockForge Agent <dockforge@local>`。
- 关键决策：ADR-0002（改用纯 Win32 + DComp + D2D，依据原 ADR-0001 回退条款）；ADR-0003（vendoring nlohmann/json 3.11.3 + Catch2 3.5.4）。
- 证据：无（环境勘察结论已写入 DECISIONS.md / STATE.json.toolchain）
- 遗留：gh CLI 未登录 → 无 GitHub remote，已记 BLOCKERS.md B-001；本地 commit 正常。
- 下轮开始于：T-001

## 2026-10-23T23:20:00+08:00 | T-001 | DONE
- 做了什么：CMake + MinGW 构建链路跑通（`tools/build.ps1 -Action all`）；无边框置顶窗口 + Ctrl+Q 退出；core/paths + core/log 基础层落地；Catch2 测试目标接入（3 用例 8 断言全通过）。
- 证据：evidence/T-001/{build.log,test.txt,verify.md,win.png}
- 关键缺陷（已修）：WS_EX_NOREDIRECTIONBITMAP 使 GDI 绘制不可见；Logger::Init 持锁重入死锁；_wfopen_s 独占句柄；Catch2 在 UNICODE 下生成 wmain 链接失败。
- 遗留：GitHub remote 仍未配置（gh 未登录），push SKIPPED。
- 下轮开始于：T-002

## 2026-10-23T23:30:00+08:00 | T-002 | DONE
- 做了什么：`src/core/config.{h,cpp}` 落地（C-01 全 11 键 + 钳制 + 自愈回写 + 未知键保留 + 原子写）；`src/main.cpp` 接线配置，窗口尺寸/文案随配置变化；Catch2 扩到 13 用例 92 断言。
- 证据：evidence/T-002/{build.log,test.txt,verify.md,runtime.log,default-config.json,normalized-config.json,win.png}
- 关键缺陷（已修）：浮点序列化泄漏二进制噪声（`1.8f`→`1.7999999523162842`）；MinGW `std::ifstream` 不收 `wstring` 路径（改 Win32 IO）；`ReadEnum` 用 `auto` 形参导致模板推导失败（改显式 `Enum` 模板）。
- 遗留：热重载属 M4；`behavior.*` 仅解析未生效。
- 下轮开始于：T-003

## 2026-10-23T23:35:00+08:00 | T-003 | DONE
- 做了什么：`src/core/event_bus.h`（类型安全同步分发、退订立即生效、可重入）；`src/core/frame_clock.{h,cpp}`（QPC 采样 + p50/p99）；`src/core/app_loop.{h,cpp}`（消息泵 + 高分辨率等待定时器帧节奏）；`src/main.cpp` 接入 AppLoop 与帧统计日志；`Logger::Init` 支持 `DOCKFORGE_LOG_LEVEL`（大小写不敏感）。
- 证据：evidence/T-003/{build.log,test.txt,verify.md,idle.txt,bench.txt,win.png}
- 关键缺陷（已修）：① `MsgWaitForMultipleObjectsEx` 的 timeout 受系统定时器粒度（~15.6ms）量化，请求 16ms 实得 30.98ms/35fps → 改 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 等待定时器（ADR-0006），得 16.00ms/62.5fps；② 截止时间按 `now + interval` 推进导致抖动累积 → 改固定步进 + 落后重同步；③ `ParseLogLevel` 只认大写，`debug` 环境变量静默失效。
- 遗留：`idle_this_frame_` 语义偏宽松（帧定时器到期也算 idle），T-010 动画阶段收紧。
- 下轮开始于：T-004

## 2026-10-23T23:52:57+08:00 | T-004 | DONE
- 做了什么：spike `spikes/t004_dcomp/` 跑通四模式 x 三 DPI；新增 `src/platform/composition.{h,cpp}` 并接入 `src/main.cpp`（GDI WM_PAINT → DComp + D2D + DirectWrite）；新增 `tools/alpha_probe.ps1` 差分透明采样与 `DOCKFORGE_DPI_OVERRIDE` 测试钩子。
- 证据：evidence/T-004/{build.log,test.txt,verify.md,alpha_matrix.txt,dpi96.png,dpi144.png,dpi192.png,idle.txt,handle_soak.txt}
- 关键缺陷（已修）：① `BeginDraw` 的 offset 是物理像素而 `SetTransform` 吃 DIP，144 DPI 下面板整体偏移出窗口（中心采样为 TRANSPARENT）→ 乘 `96/dpi`；② `ID2D1DeviceContext::GetPixelSize()` 返回合成图集尺寸而非 surface 尺寸，96 DPI 下圆角矩形溢出右边界 → 改为传 `PaintInfo{width_dip,height_dip}`；③ 用 `ShowWindow(SW_HIDE)` 取差分基线会误判（DWM 异步）→ 改杀进程取基线。
- 遗留：材质（Mica/Acrylic/模糊）未做 → T-005；WM_PAINT 每帧重建 brush，动画接入后需缓存。
- 下轮开始于：T-005

## 2026-10-24T00:45:00+08:00 | T-005 | DONE
- 做了什么：冻结 `IRenderBackend`（C-04）并实现 D2D/DWrite 后端；材质四级回退链 + 降级原因；自绘磨砂板（工作线程 BitBlt 捕获 + D2D GaussianBlur + 圆角裁剪）；修复 SetTransform 覆盖图集偏移。
- 证据：evidence/T-005/（verify.md 汇总；alpha96/144/192.txt、alpha_solid_*、contrast_*.txt、dpi*.png、tier_*.png、build.log、test.txt）
- 遗留：实时捕获 RSS 103MB 余量不足 → T-015 前优化；`plate.Draw` 每帧全量 CopyFromMemory → T-010 一并处理。
- 下轮开始于：T-006

## 2026-10-24T00:48:00+08:00 | T-006 | IN_PROGRESS
- 计划：spike 验证 `IShellItemImageFactory::GetImage` + `IShellLinkW` + `IApplicationActivationManager` + WIC PNG 编码；实现 `src/shell/app_resolver`（.lnk/.exe/UWP → 启动描述）与 `src/shell/icon_cache`（256px 位图缓存 + 落盘）；单测覆盖路径不存在、坏 .lnk、非 PE 文件。
- 待验证假设：mingw 头里 `IShellItemImageFactory` 的 `SIIGBF` 枚举值与 `SHCreateItemFromParsingName` 组合可用；`IShellLinkW::GetPath` 的 `SLGP_RAWPATH` 语义；UWP 需 `IApplicationActivationManager::ActivateApplication` 与 AUMID 枚举。

## 2026-10-24T01:20:00+08:00 | T-006 | DONE
- 做了什么：实现 `src/shell/{app_entry,app_resolver,app_launcher,icon_cache,start_menu}`。AppsFolder 五形态分类（shortcut/uwp/desktop-id/namespace/skip），三种启动路径（CreateProcess / ShellExecuteEx / ActivateApplication），三级图标缓存（内存→磁盘 PNG→GetImage）。
- 证据：evidence/T-006/（verify.md 汇总；run_*.txt 为 spike 实测原始输出，alpha_dpi*.txt 三档差分，icons_dpi*.png + px_icons_dpi*.txt，coldstart.txt，idle.txt，build.log，test.txt 295 assertions / 58 cases）
- 关键发现：① 命名空间子项 `{GUID}\name` 裸形式全部 0x80070002，必须加 `shell:AppsFolder\` 前缀；② 裸桌面 id 用 ActivateApplication 返回 E_INVALIDARG，必须 ShellExecuteEx；③ `IContextMenu::InvokeCommand` 只能按菜单序号；④ `GetPath(SLGP_RAWPATH)` 返回未展开的 `%windir%`，必须 ExpandEnvironmentStringsW；⑤ 上一版把 124 项丢进 skipped 是分类错误，实际是 69 裸 id + 54 命名空间子项。
- 工具修正：`tools/sample_pixels.ps1` 原实现忽略 `-Image` 永远抓屏（报告与文件不符＝伪造证据），已改为从文件读取；新增 `tools/fix_encoding.ps1` 修复 PowerShell 5.1 Set-Content 默认 ANSI 造成的 UTF-8/GBK 混合编码。
- 遗留：帧 p99 20 个窗口里 5 个 > 16.7ms（avg/p50 稳定）→ T-006a；AppsFolder 扫描 450ms 占冷启动主要成本 → 后续可缓存到磁盘。
- 下轮开始于：T-007

## 2026-10-24T01:40:00+08:00 | T-007 | DONE
- 做了什么：新增 `src/platform/display.{h,cpp}`（EnumerateDisplays/PrimaryDisplay/DisplayForWindow/DisplayForPoint/ComputeDockPlacement/DipToPixels/PixelsToDip）+ `tests/display_test.cpp`（13 用例）；`main.cpp` 放置逻辑收敛为单一 `apply_placement` lambda，`WM_DPICHANGED` 以 `wParam` 为权威 DPI 重算尺寸与位置；新增 `DOCKFORGE_TARGET_DISPLAY` 与 `DOCKFORGE_SIMULATE_DPI` 两个测试钩子；`tools/simulate_dpi.ps1`。
- 证据：evidence/T-007/（verify.md 汇总；run_{enum,window,move,simulate*}.txt、simulate_dpi{96,144}.txt、dpi{96,144,192}.png + px_dpi*.txt、alpha{96,144,192}.txt、edge_{bottom,left,right}.png、idle.txt、bench.txt、build.log、test.txt 380 assertions / 72 cases）
- 关键发现：① 本机单显示器（2880x1920，有效 192 DPI / 原始 243 DPI），真实跨屏 DPI 转换不可复现；② `GetSystemMetricsForDpi(SM_CXSCREEN)` 与 DPI 无关；③ `AdjustWindowRectExForDpi(WS_POPUP)` 是 no-op；④ **`WM_DPICHANGED` 无法跨进程注入**（PostMessage 返回 1159，SendMessage 返回 0 但不执行；WM_CLOSE 可跨），验证必须在进程内；⑤ 放置必须用 `work` 而非 `monitor`，否则压到任务栏下。
- 诚实边界：混合 DPI 仅「逻辑通过 + 处理器路径验证」，COMPATIBILITY.md 未标记实测通过；`GetDpiForWindow` 在合成消息后仍返回 192，证据文件 note 行已记录。
- 遗留：T-006a 的 p99 尾延迟本轮 12 个采样窗未复现（全部 ≤16.68ms），保留待复现再动。
- 下轮开始于：T-008
## 2026-10-24T02:35:00+08:00 | T-008 | DONE
- 做了什么：新增 `src/dock/layout.{h,cpp}`（LayoutParams/IconLayout/LayoutSummary、FalloffWeight、ComputeScale、BaseContentWidth、IconCountForWidth、零分配 LayoutDock、FromConfig）+ `tests/layout_test.cpp`（15 用例，含契约逐字 O(n²) 对照 oracle）+ `tools/layout_bench.cpp`；CMake 新增 `dockforge_bench` 目标（`layout_bench`）。
- 证据：evidence/T-008/（verify.md 汇总、bench.txt、test.txt 1316 assertions / 87 cases、build.log）
- 关键发现：① 指针钳到 `[0, base_width]` 是错的——首图标中心在 `w/2`，边缘图标峰值只有 1.5556 而非 1.8，且峰值索引单调性被破坏；必须钳到首尾图标中心跨度。② 热路径每次 `std::vector<float> gain` 分配违反契约，129.88ms → 80.96ms。③ 半径外跳过 `pow()` 后再降到 7ms 量级。④ benchmark 单位必须先声明：C-05 的「一次求值」是单图标公式项，不是整条 Dock。
- 遗留：GPU 顶点变换属 T-010；`IconCountForWidth` 与实际取图标数量在 T-009 接。
- 下轮开始于：T-009

## 2026-10-24T01:56:00+08:00 | T-009 | DONE

- 做了什么：`src/dock/dock_view.{h,cpp}` 承载 tile 构建/几何/绘制，`src/main.cpp` 接上并把窗口尺寸交给 `dock::ComputeGeometry` 单一来源；`IRenderBackend` 新增 `BitmapView` + `DrawBitmap`，D2D 侧按内容身份缓存 `ID2D1Bitmap1`；补 `shell::ToString(AppKind)` 缺失定义。
- 证据：`evidence/T-009/{verify.md,build.log,test.txt,idle.txt,placed.txt,dpi96.png,dpi144.png,dpi192.png,slots96.txt,slots144.txt,slots192.txt,px_icons.txt,px_scan.txt,probe96.txt,probe144.txt}`
- 实测：12 tile 全部取到图标，三档 DPI `ALL_SLOTS_DRAWN`；窗口 684x68 / 1026x102 / 1368x136 与 `round(684 DIP × dpi/96)` 完全一致；空闲 CPU 0.0029%、RSS 77.83MB、句柄 372→364；帧 p99 16.59–16.64ms。
- 遗留：悬停放大（T-010）、点击行为（T-011）、运行指示（T-012）、拖拽（T-013/T-014）、缩略图（T-015）均未开始。`DockPaintInput::hover` 已预留但窗口不随悬停扩张，属 T-010 决策。
- 下轮开始于：T-010

## 2026-10-24 | T-010 | DONE

- 做了什么：`src/platform/hit_region.{h,cpp}` 新增 `CreateDockRegion`/`ApplyWindowRegion`；`src/dock/dock_view.*` 新增 `DockHover`/`AdvanceHover`/`UpdateLayout` 与满倍率几何；`src/main.cpp` 接入 `WM_MOUSEMOVE`/`WM_MOUSELEAVE`/tick 合并/region 两态切换，新增 `paint_clock` 分项打点；`tests/dock_hover_test.cpp` 14 用例。
- 证据：`evidence/T-010/`（verify.md、dpi{96,144,192}.{txt,idle.png,measure.txt,region_idle.txt,region_hover.txt,diff.txt}、bench_hover.txt、idle.txt、region_click.txt、region_clickthrough_idle.txt、test.txt、build.log）
- 关键发现：① `HTTRANSPARENT` 跨进程不穿透点击，`SetWindowRgn` 才是唯一机制，且它同时裁自绘 DComp 内容；② 每条 `WM_MOUSEMOVE` 重绘使 p99 到 18.2ms，每帧 `SetWindowRgn` 使 p99 到 17.3ms，两者都修掉才降到 16.54ms；③ `DOCKFORGE_DPI_OVERRIDE` 下所有验证工具必须从窗口高度反推 scale，问 `GetDpiForWindow` 得到的是真实 DPI 不是布局 DPI。
- 遗留：混合 DPI 双屏仍无法真机验证（单显示器，同 T-007）；点击穿透只在 spike 双窗口场景验证机制，未在真实第三方应用上验证。
- 下轮开始于：T-011

## 2026-10-24 | T-011 | DONE

- 做了什么：`src/shell/window_registry.{h,cpp}`（五条窗口筛选、`pid`/`app_pid` 分离、AUMID 优先匹配、文件身份比较）、`src/shell/window_actions.{h,cpp}`（三分支状态机 + `ShowWindowAsync` + ALT 回退）、`DockPainter::HitTest`、`DOCKFORGE_PIN_APPS` 验证钩子、`WM_LBUTTONDOWN` 接入；`tests/shell_window_test.cpp` 11 用例；新增 `tools/click_probe.ps1`、`tools/click_bench.ps1`、`spikes/t011_click/`。
- 证据：`evidence/T-011/`（verify.md、click_tile0.txt、click_tile4.txt、click_restore.txt、click_uwp.txt、click_uwp_minimize.txt、click_bench.txt、idle.txt、enum.txt、frame.txt、aumid.txt、procs.txt、packaged.txt、fg_plain.txt、fg_altkey.txt、fg_click.txt、activate.txt、minimize.txt、path_mismatch.txt、test.txt）
- 关键发现：① 打包应用的顶层窗口属于 `ApplicationFrameHost.exe`，必须 `EnumChildWindows` 找跨进程子窗口才有应用 pid；② AUMID 与路径不能「或」匹配，所有打包应用共享 frame host 路径，退化会让计算器与设置串味；③ scoop 的 `current` junction 让条目路径与进程路径永不相等，必须比 `GetFileInformationByHandle` 的卷序列号+文件索引；④ `ShowWindow` 同步等待目标应用，实测 p99 494.59ms、10/20 次超预算，改 `ShowWindowAsync` 后 p99 9.89ms、0 次超预算；⑤ `SetForegroundWindow` 从后台被拒、`AttachThreadInput` 无效，但 `WM_LBUTTONDOWN` 里直接调用成功——点击路径本身持有前台权限。
- 遗留：跨屏激活未测（单显示器）；UWP 分支只覆盖计算器；`route` 的 restore/raise 依赖动作前采样，第三方并发最小化会标错（不影响行为）。
- 下轮开始于：T-012


## 2026-10-24T03:12+08:00 | T-012 | DONE

- 做了什么：新增 `src/dock/running_tracker.{h,cpp}`（三值 `TileRunState`、双时钟、300ms 轮询钳制 50..5000）；`dock::DockTile` 加 `run_state`；`DockPainter` 在板底部内边距画圆点（前台 0.95/1.0x，后台 0.45/0.85x）；`ComputeGeometry` 暴露 `baseline_dip`；`src/main.cpp` 接线 tick 与点击后 `Invalidate`；新增 `DOCKFORGE_RUNNING_POLL_MS` 验证钩子。
- 证据：`evidence/T-012/`（`verify.md`、`indicator_states.txt`、四态 png + 采样、`ab_tracker_cost.md`、`idle.txt`、`click_*.txt`）
- 关键发现：
  1. 两个时钟不可合并 —— 存在性 300ms 轮询（0.5ms/次），前台每帧读（亚微秒）。合并后 alt-tab 高亮滞后 300ms。
  2. 圆点必须画在板的 10 DIP 底部内边距里。新增 7 DIP 的带会让板和窗口一起长高、屏幕上每个 Dock 位移，并让所有从窗口高度反推 scale 的验证工具失效。
  3. 交替 A/B（poll 300 vs 5000）证明 tracker 不是帧尾延迟原因：差值 0.016ms，同配置波动 0.018ms，符号相反。
  4. 三个工具缺陷：探针漏加行原点、探针硬编码板色（solid 仍混合 8% 背景）、`click_probe.ps1` 退出即杀进程让「截不到图」看起来像「Dock 崩了」。
- 遗留：帧预算判 WATCH（归入 T-006a）；文档类条目不显示圆点（真实归属）；多窗口逐窗口指示依赖 T-015。
- 下轮开始于：T-013


## 2026-10-24T04:20+08:00 | T-013 | DONE

- 做了什么：新增 `src/dock/drag_controller.{h,cpp}`（四相状态机、4 DIP 阈值从按下点测、550ms 长按移除确认）与 `src/dock/tile_order.{h,cpp}`（身份归一、托管/非托管排序、`MoveTile` erase+insert、`CommitOrder` 立即写盘）；`core::DockConfig` 加 `tiles`/`hidden` 两个身份数组；`DockPainter` 加 ghost + 插入线 + `IdleCentersDip` 静止栅格；`ConfigPath()` 支持 `DOCKFORGE_CONFIG_PATH` 覆盖并隔离测试；`src/main.cpp` 把点击从按下移到抬起，接 `WM_CANCELMODE`、拖拽期 region 扩整窗。
- 证据：`evidence/T-013/`（`verify.md`、`drag_reorder.txt`、`drag_eject.txt`、`drag_eject_abort.txt`、`drag_visual_{96,144,192}.txt` + 六张 png、`drag_bench.txt`、`idle.txt`、`test.txt`、spike 四组 `run_*.txt`）
- 关键发现：
  1. 插入槽不能用放大后的槽中心。放大布局是鼠标位置的函数，喂回 `InsertionIndexFor` 形成反馈环，实测把 `0 -> 4` 报成 `0 -> 3`。改用解析式静止栅格 `IdleCentersDip`。边界同时从 `>` 改 `>=`（指针压在槽中心就该占该槽）。
  2. `GetCapture()` 返回的是**调用线程**的捕获窗口，从探针进程读永远是 0。首轮 `capture_taken=False` 差点被当成"捕获没建立"去改设计；改读 app 自报的 `capture=1 match=1` 才确认捕获成立。
  3. 移除必须有确认。Dock 是 `WS_EX_NOACTIVATE`，弹确认框会抢焦点（比误删更糟），改用长按 550ms + ghost 随进度淡出。
  4. `core::FrameClock` 是 1000 样本滚动窗，启动期首帧 309ms 会在窗内滞留 16 秒，把拖拽 bench 的首窗 p99 抬到 19.63ms。等 `frames=1000` 再取基线后降到 16.69ms。
  5. `CommitOrder` 的单元测试曾写进开发机真实 `config.json`，让下次启动 Dock 只剩 1 个 tile。测试隔离必须做在路径层，不能靠用例自觉。
- 遗留：拖拽视觉验证依赖人工传几何参数（工具不做独立校验）；单显示器无法验证跨屏拖拽；插入线判据（≤10px 亮列）在图标集变化后需复核。
- 下轮开始于：T-014

## 2026-10-24T04:40+08:00 | T-014 | DONE

- 做了什么：新增 `src/dock/drop_target.{h,cpp}`（`OleSession` 引用计数 RAII、`DropPointer`、`DropSink` 三个 `std::function`、`DockDropTarget : IDropTarget`、`InsertIndexFor`）与 `src/shell/drop_resolver.{h,cpp}`（`DropKind` 五值、`IsDockable`、`ClassifyDroppedPath(s)`、`ResolveDroppedPath`）；`AppKind` 加 `Folder` 与 `LaunchFolder`（`ShellExecuteExW`）；`DockPainter` 加 `IdleCentersDip` 自由函数与 4 DIP 竖线 caret（近白/暖红）；`main.cpp` 用 `dock::OleSession` 取代 `CoInitializeEx`，接 `drop_sink` 三段回调、region 拖入期扩整窗、tick 合并 `drop_moved`。
- 证据：`evidence/T-014/`（`verify.md`、`drop_cases_dpi192.txt`、`drop_bench.txt`、`idle.txt`、`drop_caret_probe_{accept_dpi96,accept_dpi144,accept_dpi192,reject_dpi192}.txt`、`drop_diff_*.txt`、八张 png、spike 21 个 `run_*.txt`）
- 关键发现：
  1. `RegisterDragDrop` 在 `CoInitializeEx` 之后返回 `0x8007000E E_OUTOFMEMORY`，只有 `OleInitialize` 返回 `S_OK`。错误码是误导性的，真实含义是"OLE 没初始化"；三组变体（含 `COINIT_DISABLE_OLE1DDE`）全部失败。
  2. region 外的 `DragEnter`/`Drop` **完全不投递**，边界严格等于 region 顶边（39% 收不到、40% 收到）。进入后不扩 region 则指针上移触发 `DragLeave` 且 `Drop` 永不触发，源进程的 `DoDragDrop` 卡死须强杀。
  3. 落点又栽在"几何与 DPI 不同源"上：`UpdatePointer` 读 `GetDpiForWindow`（192）而行按 `DOCKFORGE_DPI_OVERRIDE=96` 排布，`x=188.5dip` 把 tile[6] 算成 tile[3]。这是 C-10 那条规则的第二次发作，第一次是 T-004 的 `BeginDraw` offset。
  4. 每个 OLE 回调 `InvalidateRect` 让 `p99_mean=17.284ms`、19/20 窗超预算；tick 合并后 `16.512ms`、0 窗超。`paint_p99_max=5.320ms` 证明超支在消息洪流不在绘制。同一条规则第三次应用。
  5. caret 探针本身错两次：整帧 bbox 被行回落主导（96 DPI 报 `0,57..754,105` 跨整行），必须按列统计并只认 ≤12px 宽的单 run；颜色取 run 首个合格列会读到抗锯齿边缘 `(46,28,29)`，把 reject 判成 accept，必须取 run 内峰值像素 `(219,98,89)`。
- 遗留：多文件拖入只有单测无端到端截图；跨屏拖入未测（单显示器）；blur 档无法截图（`WDA_EXCLUDEFROMCAPTURE`），视觉证据一律 solid。
- 下轮开始于：T-015

## 2026-10-24T06:10+08:00 | T-015 | DONE

- 做了什么：新增 `src/platform/preview_window.{h,cpp}`（自有窗口类 `DockForgePreviewWindow`，`WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE` + `WS_POPUP`，禁 `NOREDIRECTIONBITMAP`，隐藏不销毁）、`src/shell/thumbnail_host.{h,cpp}`（`DwmRegisterThumbnail` 生命周期、每 tick 1 个的分帧注册、dest 矩形物理像素、`ReleaseAll` 唯一注销入口）、`src/shell/window_list.{h,cpp}`（`WindowsForEntry` 复用 `IdentityOf` + 窗口侧身份，不另写匹配）、`src/dock/hover_timer.h`（400ms dwell latch）；`main.cpp` 接入 tick 内弹出/续推、`WM_MOVE` 跟随、`WM_DPICHANGED` 隐藏、`WM_MOUSELEAVE`/`WM_LBUTTONDOWN` 取消、收尾 `ReleaseAll` 先于 `preview.Destroy()`；新增 `tests/preview_test.cpp` 24 例。契约 C-17a..C-17l 与 ADR-0020 已冻结。
- 证据：`evidence/T-015/`（`build.log`、`verify.txt`、`bench.txt` + spike 的 12 个 `tmp_*.txt` / `run_plain_3.txt` / `tmp_full.png`）
- 关键发现：
  1. 冷 `DwmRegisterThumbnail` 实测 219–609ms/槽（8 槽共 594ms），比 16.7ms 帧预算高一两个数量级。因此弹窗先出框、注册按每 tick 1 个补齐，8 窗口最多约 8 帧填满；同步注册会正好在用户索要预览的那一帧吹穿预算。
  2. 同目标窗注册多槽存在经验上限：110x70/120x90/130x70/130x130/140x70 DIP 全 8/8；150x70/160x70/170x70 报 7；180x70 报 6；200x130 只 4/5/6。已排除注册节奏、重注册、脏区、源重绘、TOPMOST、布局方向、注册顺序、槽高；非总像素预算、非窗口宽度。未定因，工程取 110x70+gap8 绕开（spike 三次全 8/8 稳定）。
  3. `rcDestination` 必须是 dest 客户端物理像素，传 DIP 在 200% 屏整体半偏移——与 T-004 `BeginDraw` offset、T-014 `UpdatePointer` 是同一类错误的第三次。
  4. 最小化源窗后 DWM 保留末帧（`best_delta=0`），应用层无需缓存；但进程退出后缩略图空白，故来源列表每次 dwell 从 `RunningTracker::Windows()` 重取。
  5. 预览窗不能复用 `platform::Window`：同类 WndProc 在 `WM_DESTROY` 时 `PostQuitMessage` 会结束 dock 消息循环，且其消息 hook 属主 dock。
  6. `ThumbnailHost::PumpPending` 对 `!dest` 或 `!IsWindow(source)` 必须 `break` 保留队列而非 `erase`：erase 会把无 dest 期的排队静默清空（单测 `PendingCount==2` 抓到），且死源永不重试。
- 遗留：本轮按用户要求不实机验证，端到端四项（400ms 日志、多窗口 N 槽、Calculator UWP 帧源、帧预算 p99）待 `tools/preview_probe.ps1` + `tools/hover_bench.ps1` 在实机轮补齐；`GetGuiResources` 前后一致已在 spike 验证（40 轮 GDI 8→8 / USER 19→19），产品侧需实机复核一次。
- 下轮开始于：T-016
## 2026-10-24T06:40+08:00 | T-016 前置 | DONE（脚本，未执行）

- 做了什么：为 T-016 实机验证写了三个脚本本体（**均未运行**，用户 m00005 指令仍在生效）：`tools/preview_probe.ps1`（三段 dwell 用例：250ms 无日志 / 900ms 有 / 离开后重新武装，快门严格等 `preview shown tile=` 日志出现再开）、`tools/hover_bench.ps1`（重写为 A/B 交替：`DOCKFORGE_PREVIEW=1` vs `0`，指针停在单 tile 不动，等 `frames=1000` 后取 p99，差值低于 0.018ms 噪声底判"无信号"）、`tools/preview_handles.ps1`（40 轮显隐 + `GetGuiResources` GDIOBJECTS/USEROBJECTS 前后对比，同时统计 `preview shown`/`hidden` 行数确认每轮都真的走过两半）。
- 同步更新：CONTRACTS.md C-17j 验证表补 `preview_handles.ps1` 行并把帧预算判据改成 A/B + 噪声门限；C-17j 下新增三条 hover_bench 实现约束（快门由日志驱动 / A/B 交替 / 等 frames=1000）；TASKS.md T-016 DoD 标注每项的前置状态。
- 未做（需实机放行）：三个脚本的执行、多窗口 4/10 窗用例（还需先写"开 N 个同 app 窗口"的前置脚本）、最小化用例、Calculator UWP 帧源确认。

## 2026-10-24T22:35+08:00 | T-016 实机验证 | DONE（五条 DoD 全部实测通过）

- 做了什么：把三个验证脚本从 `SetCursorPos` 驱动改成**文件通道**驱动（`DOCKFORGE_HOVER_PROBE_FILE` 指向一个文本文件，内容是 tile 索引），跑通全部五条 DoD，并在过程中抓到一个真实产品缺陷。
- 为什么必须换通道：`SetCursorPos` 赢不了与真人的竞争。实测 21:12 日志里 `hovered=11 active=1 elapsed` 从 0 累积到 125.9ms 后被清零——用户的物理鼠标穿过 dock；21:05 整轮 `active=1` 行数为 0（指针整轮没进 dock）。探针只替换 hover 的**来源**，HitTest、400ms dwell、窗口查找、弹窗放置、每一次 `DwmRegisterThumbnail` 全部走生产代码，默认关闭。
- 产品缺陷（T-015 引入，本轮修复）：`shell::WindowsForEntry` 在 `MatchesEntry` 之前加了一层字符串预筛 `IdentityOfWindow(window) != identity`。7-Zip 的 tile 身份是 `...\scoop\apps\7zip\current\7zFM.exe`，进程报 `...\26.00\7zFM.exe`，字符串永不相等 → `MatchesEntry` 从未执行 → 三窗口的 app 永不弹预览。这正是 known_gotchas T-011 记录的 scoop junction 陷阱，在预筛里重犯。修法是**彻底删除预筛**，直接 `MatchesEntry`；成本界由 `SameExecutable` 承担（仅在 lowercase 路径不等时才碰文件系统，忙桌面约 10-20 个 app 窗）。单测同步改成条件强断言：原版只断言「匹配到的必须是 hwnd 1」，空结果也通过，等于放行该 bug。
- 自己引入的性能缺陷（本轮修复）：诊断行 `DF_DEBUG("preview", "tick hovered=... windows=%zu", ...)` 每帧执行，其中 `WindowsForEntry → MatchesEntry → SameExecutable → CreateFileW` 是同步文件 I/O。bench 实测 `paint n=2` 而 loop p99=19.7ms——测的是这行日志不是 preview。改成 200ms 节流。
- 关键发现：
  1. `frames>=1000` 是取 p99 的硬前提。FrameClock 是 1000 样本滚动窗，之前窗口仍含启动 309ms 首帧。实测同一进程 frames=63..377 读 p99=19.6ms，frames=1000 后读 16.5ms。差 3ms 足以把 PASS 读成 FAIL。
  2. 帧预算必须对**裸定时器地板**比较，不能对 16.7ms 硬比。本机裸 `CreateWaitableTimerExW(HIGH_RESOLUTION)` + `timeBeginPeriod(1)` 在忙桌面下 p99=19.226ms，本身就超门槛。用 A/B 交替 + 同配置重复间 spread 作噪声底，得到 `p99_delta_ms=0.909 noise_floor_ms=3.326 → NO_SIGNAL`，同时 quietest preview_on p99=16.5ms ≤ 16.7ms 记 WITHIN。
  3. `GetGuiResources` 是瞬时读，hide 刚结束不是稳态。实测空闲进程连续 12 次读全部 gdi=1 user=6，而 hide 后单次读会读到 user=7。必须 `Get-Settled` 取 8 次采样最小值，且加 2 轮预热把首帧惰性分配（GDI 0→1、USER 6→7）排除在基线之外。断言改成稳态对比（中点 vs 终点增量为 0），否则「泄漏」与「一次性分配」无法区分。
  4. 差分必须裁到弹窗自身矩形再算。在「最宽可能弹窗」区域（1968x236）上算，桌面稀释使 ratio 只有 0.25-0.26；裁到弹窗矩形后 ratio 才反映真实合成（单窗口 1.0、三窗口 0.8475、UWP 0.9998）。
  5. 最小化保留末帧得到产品侧复核：三窗口 `SW_MINIMIZE` 后重新 dwell，与可见态同矩形差分 `ratio=0.0143`（498/34780 像素变化），`colors=406 mean_luma=171.7`——不是纯黑板。DWM 确实保留末帧，应用层不需要缓存。
  6. 槽位数按 `kMaxThumbnails` 钳制是正确行为：十窗口 `windows=10` 但 `registered_sources=8 of expected=8`，弹窗 1920x188。探针若拿原始窗口数比较会误判 FAIL。
  7. UWP 的 `hwnd` 是 frame host（pid 9680 `ApplicationFrameHost`），真实 `app_pid=2152`，`class=ApplicationFrameWindow`。C-17g 的「同时记录两者」在实机成立。
- 工具链踩坑（PowerShell 5.1，三条都是致命且静默的）：
  1. **变量名大小写不敏感**。`function Compare-Shots([string]$A, ...)` 里写 `$a = New-Object Bitmap($A)`，`$a` 就是参数 `$A`，`[string]` 类型约束把 Bitmap 强制转回路径字符串 → 报 "The property 'Width' cannot be found on this object"。参数名必须 `$PathA/$PathB`。
  2. `List[string] += 数组` 会把变量重绑成 `Object[]`，之后 `.Add()` 抛 "Collection was of a fixed size"。必须 `foreach ($l in $x) { $report.Add($l) }`。
  3. `Get-DockRect([int]$Pid)` 与只读自动变量 `$PID` 冲突 → "Cannot overwrite variable Pid because it is read-only"。改 `$TargetPid`。
  4. 无 BOM 的 `.ps1` 按 ANSI(936) 解码，中文字面量在解析阶段就炸。定型的做法是脚本正文纯 ASCII，所有中文放外部 UTF-8 文件用 `ReadAllText($p, $utf8)` 读入。
  5. `$env:X` 必须在 `Start-Process` **之前**设置，否则子进程拿不到。`$startOffset = (Get-LatestLog).Length` 同理必须在启动前取。
  6. 探针进程未设 DPI awareness 时 `GetWindowRect` 与 `CopyFromScreen` 都返回虚拟化坐标——实测 dock 真实 1334,1595 214x214 被报成 667,798 107x107，所有截图落在屏幕另一半。必须 `SetProcessDpiAwarenessContext(-4)`。
- 环境事实：单屏 2880x1920@192 DPI（raw 243），work=0,0,2880,1824。多窗口 Win32 app 只有 7-Zip（3-10 窗口，`7-Zip::FM`）与 explorer（`CabinetWClass` 但 tile 走 AUMID 空间，属 C-14f 记录的真实归属）。Sublime/Chrome 无法开出第二窗口（`--new-window`/`-n`/`AttachThreadInput+Ctrl+Shift+N` 均无效）。Notepad 是 UWP，`GetApplicationUserModelId` 对 notepad.exe 返回 `0x00003D57`（APPMODULE_ERROR_NO_APPLICATION）。
- 证据：`evidence/T-016/`（verify.txt 五条 DoD 逐条 + build.log + hover_bench.txt + preview_handles.txt + 顶层与 four/minimized/ten/uwp 四个子目录的 dwell 日志与对照 PNG）。

## 2026-10-24T23:50:00+08:00 | T-040 | DONE

`src/shell/launchpad_index.{h,cpp}` 新建：三来源聚合（AppsFolder / `launchpad.extra_paths` / `launchpad.folders`）、按 C-15a 身份去重、`CompareStringEx` collation 排序、内存缓存、`ReadDirectoryChangesW` 监视 + 500ms 防抖。`src/shell/app_identity.{h,cpp}` 把身份规则从 `src/dock/tile_order.cpp` 抽到 shell（C-06 要求 shell 不依赖 dock），dock 侧保留同名薄包装。

- 实测（本机 2880x1920@192 DPI）：`ScanAppsFolder entries=215 shortcut=49 uwp=43 desktop_id=69 namespace=54 skipped=1`；`items=208 accepted=208 skipped=7 upstream=1 pathfail=0 scan_ms=360.9`。灌入机器级 Programs 目录后 `items=280 extra=72 dup=29 unique_identities=280`，集合大小等于元素数，去重成立。
- 重建耗时实测 360.9 / 373.2 / 477.4 / 522.0 / 548.8 / 719.4 / 1044.9 ms，差异来自磁盘缓存冷热。分段计时：`ScanAppsFolder 513.2ms` / `FindShortcuts 5.0ms` / `ResolvePath 119 of 121 in 127.3ms`——AppsFolder 扫描占 93%，所以 Launchpad 必须异步预热，不能打开时同步扫。
- 两个真实缺陷：
  1. **文档混入网格**。WinRAR 开始菜单目录有一条指向 `whatsnew.txt` 的快捷方式，`ResolvePath` 成功、`launchable()` 为真（探针实测 `resolve=none kind=Shortcut launchable=1`），于是作为应用进入网格。`launchable()` 回答的是「能不能启动」，而 ShellExecute 能打开 `.txt`。修法：`LooksLikeApplication` 按 kind 分派，`Shortcut` 与 `NamespaceItem` 都要求目标尾缀是 `.exe/.msc/.cpl/.bat/.cmd/.com`。效果 `items 215→208`，被拒的 7 条全是 IDM/WinRAR 的 `.chm`/`.txt` 手册，零误杀（`regedit.exe`/`charmap.exe`/`comexp.msc`/`services.msc` 全保留）。
  2. **统计口径混算两个总体**。`stats_.skipped` 同时收了 `ScanAppsFolder` 内部丢的 1 条与索引内拒的 7 条，导致 `215-8=207` 而 `items=208` 差 1。拆成 `skipped`（本索引拒绝）与 `apps_folder_skipped`（上游丢弃），新增 `apps_folder_accepted` 与 `ExpectedItemCount()`，`Rebuild` 末尾加 census 自检。第一次拆完仍算错：`ExpectedItemCount()` 写成 `accepted+extra-dup`，而 `extra_path_count` 本就 post-dedup，`dup` 是重复扣减（with_extra 场景 `208+72=280` 正确，`208+72-29=251` 差 29）。最终公式 `accepted+extra_path_count`，`dup` 只用于解释差额。
- 契约错误：CONTRACTS.md:851 原写 `NORM_LINGUISTIC_CASE`，该宏在 Windows 头里不存在（`winnls.h` 只有 `NORM_LINGUISTIC_CASING 0x08000000`）。已改。用 `build/probe_cmp.cpp` 对比三种 flag 后确认：`SORT_DEFAULT` 与 `SORT_DEFAULT|NORM_LINGUISTIC_CASING` 对 `apple`/`Apple` 结果完全相同（都是 -1，小写在前），只有 `NORM_IGNORECASE` 才让两者相等——所以「大小写不敏感排序」的直觉断言 `==0` 是错的，测试改成「反称 + 非零 + 大小写不主导主序」。
- 监视器三个内存安全缺陷：① 把事件 `HANDLE` 强转 `LPOVERLAPPED` 传给 `ReadDirectoryChangesW`（内核会往句柄值当地址写状态）；② `vector<Watch>` by-value + 先 arm 再 `push_back`，扩容后内核仍写旧栈地址，监视器永不触发；③ 线程循环用局部 scratch buffer，上一次未完成时被下一次复用。最终形态：每个 Watch 自有 `OVERLAPPED` + 64KB buffer，`vector<unique_ptr<Watch>>` 保证地址稳定，`~Watch` 先 `CancelIoEx` 再 `CloseHandle`，单线程 `WaitForMultipleObjects` 服务所有 root。
- 测试：`tests/launchpad_test.cpp` 22 例 / 780 assertions（空目录、不可读路径、损坏 `.lnk`、UWP AUMID 键、重复身份、排序稳定性、防抖三例、搜索三例、文件夹三例、文档拒绝、census 自洽、watch roots）。全量 `All tests passed (2681 assertions in 225 test cases)`。
- 顺带修掉 T-016 遗留的测试基础设施缺陷：`tests/shell_icon_test.cpp` 的 `TempCacheDir()` 用「PID + 计数器」命名，Windows 复用 PID 会撞上上一轮遗留目录 → 磁盘命中 → `extractions=0` → 「必须重新提取」断言反转。实测 25 次全量里失败 1 次，本机 `%TEMP%` 积压 648 个残留目录。修法：名字加 `GetTickCount64()`，加 `CacheDirReaper` 退出时清理。连跑 12 次全量 0 失败、残留 0 个。
- 证据：`evidence/T-040/`（build.log 13 行 warning=0 error=0；verify.txt 六条 DoD 逐条 + 两个缺陷根因 + 诚实边界 + 探针原始输出附录）。
- 诚实边界：逐条目增量更新未做（监视器只产生「有变化」信号，触发整表重建）；`launchpad.folders` 的 UI 交互未验证（T-042 之前无窗口消费）；跨屏与换系统语言的排序未测。

## 2026-10-25T00:36:00+08:00 | T-041 | DONE

**产物**：`src/dock/launchpad_layout.{h,cpp}`（纯数学网格/分页/动画曲线）、`src/dock/launchpad_view.{h,cpp}`（tile 模型 + FNV-1a 64 图标键 + painter）、`src/platform/overlay_window.{h,cpp}`（独立 overlay 窗口）、`src/dock/launchpad.{h,cpp}`（控制器）、`tests/launchpad_layout_test.cpp`（15 用例）、`tools/launchpad_probe.ps1`。

**实测数据**（2880x1920@192 DPI，build 26300）：
- 网格：`columns=10 rows=4 per_page=35 cell=105.2x120.0 origin=64.0,205.0 icon=96.0 viewport=1440x960`；C-18 公式 `floor((1440-128)/(96+26))=10`，与实得一致。
- 分页：208 项 → 6 页；`page_diff ratio=0.1841`。
- 悬停：`hover_diff ratio=0.0122`，`kLaunchpadHoverScale=1.10`。
- 点击启动：`launch 7-Zip -> none route=CreateProcess pid=24908`，随后 `closed`。
- 帧预算：原生 `p99_min=16.58 p99_median=16.59` → WITHIN_16_7；150% override `p99_min=19.36 p99_median=20.18` → 与裸定时器地板 19.226ms 不可区分，未宣称达标。
- 测试：`All tests passed (2914 assertions in 240 test cases)`。

**修掉的四个真实缺陷**：
1. overlay 只画第一帧且第一帧全透明（Tick 返回 dirty 但没人 Repaint；A/B ratio 0.0044）。
2. 首次 Open 卡 1797ms（图标提取在 Open 内；改 WarmTiles 预热后 32.0ms）。
3. 网格 cell 宽度等于 pitch，列间空隙成了点击目标（单测暴露）。
4. Launchpad 自调 EnumerateDisplays 绕过 apply_dpi_override，DPI 覆盖对它不生效。

**探针的三个方法论修正**：快门由 app 日志驱动且等「已绘制」而非「命令已消费」（否则报 0.0041 而真实 0.1841）；不得用物理 rect 反算列数（scale=2 必错）；`-NoProfile` 下必须显式 Add-Type System.Windows.Forms。

**诚实边界**：混合 DPI 双屏与 per-display 悬停受单屏硬件限制，未实机验证，移交 T-084；"模糊淡入淡出"以 scrim alpha 实现（ADR-0021 固定 solid 材质，全屏抓取 78ms/帧不可能每帧做）。

- 下轮开始于：T-042 Launchpad 搜索与文件夹分组

## 2026-10-25T09:30:00+08:00 | T-042 | DONE

**产物**：`src/dock/launchpad_search.{h,cpp}`（搜索框/chip 条纯几何 + caret）、`src/dock/launchpad_folders.{h,cpp}`（文件夹创作：创建/重命名/增删/持久化，全部经 ClampConfig）、`tests/launchpad_search_test.cpp`（12 例）、`tools/launchpad_t042_probe.ps1`；overlay 窗口新增 `pressed/released/character` 回调与 WM_LBUTTONUP 转发；main.cpp 探针通道新增 `type/backspace/clear/key/press/release`。

**实测数据**（2880x1920@192 DPI，配置隔离在 %TEMP%，未触碰真实 config）：
- 搜索：`search '7' -> 1 item(s)`，全屏差分 0.1419；`search '7zzzzprobe' -> 0 item(s)` 空状态后进程存活。
- 拖拽建夹：press cell0 -> hover cell1 -> release -> `folder created id=... items=2 saved=1`；chip 点击 -> `folder opened name=New Folder items=2`。
- 重命名：F2 -> clear -> type Play -> Enter -> `folder renamed saved=1`；Esc 取消保留旧名；Esc 逐级回退（重命名->文件夹->关闭）。
- 持久化：杀进程重启后 chip 点击 `folder opened name=Play items=2`，隔离 config 回读 `Play`/2 items。
- 探针 verdict=PASS；单测 `All tests passed (3315 assertions in 260 test cases)`；构建 0 警告 0 错误。

**修掉的真实缺陷**：
1. 承接的 WIP 未编译：缺三个 Paint 方法声明、`open_chip_index_` 引用不存在的成员、chip 开/编辑状态无法从 id 推导——改为 PaintInput 传 strip 下标（`open_chip`/`renaming_chip`），控制器 `ChipIndexOf` 解析。
2. T-041 遗留：第 3 页起图标全空（`BuildLaunchpadTiles` 按 70 截断后 `PaintCells` 仍按 `page*per_page` 直索引）。改为 tiles 预填全部条目、前 70 预提取、其余首绘懒提取（`attempted` 保证每 tile 只试一次）。
3. 逐字节 `wstring(begin,end)` 宽化/窄化会让非 ASCII 文件夹名乱码，统一改 `core::Utf8ToWide/WideToUtf8`。
4. 点击启动从按下时改为抬起时（按下移动超 14 DIP 即拖拽），否则拖拽不可能成立；探针 `ClickAt` 保持一次调用=完整手势，T-041 click 探针不变。Esc 回退链：重命名->文件夹->关闭。
5. 探针侧三错记录在案：VK_F2=113 非 117（靠 KeyPressed 入口 `key vk=` 诊断行定位，该行保留）、命令通道去重吃掉连续同名命令、单 cell 场景全屏差分被稀释必须 crop 比较。

**诚实边界**：未预热页首次翻页可能一次性格顿（冷 ~25ms/图标，一页约 900ms，仅一次）；重命名编辑器无选区模型（caret 固定末尾）；chip 条超屏宽截断无滚动；混合 DPI 双屏未验证（移交 T-084，与 T-041 一致）。

- 下轮开始于：T-050 设置窗口骨架（M4）

## 2026-10-25T10:05:00+08:00 | T-200 对标审计 + P0 缺陷修复 | DONE

**P0 缺陷（用户实测报告：所有应用图标上下颠倒）**：根因是 `IShellItemImageFactory::GetImage` 返回
BOTTOM-UP 的 DIB section（spike 实测 `dsBmih.biHeight=+192`），`icon_cache.cpp` 的 bmBits 快速路径
按存储行序复制导致全部图标颠倒，且 `SaveToDisk` 把颠倒固化进 PNG 缓存。修复：`IconBitmapFromHBitmap`
无条件走 `GetDIBits(-height)`（行序由 API 契约保证；中途试过按 DIBSECTION 符号翻转，被 top-down
单测抓到 GetObject 对负高 section 返回正值的陷阱后放弃）；`CacheKey` 引入 `v2_` 字面量前缀整体作废
v1 缓存（首次尝试 `%s` 拼前缀被宽字符 printf 截成 `v_`，已记入注释防重犯）。新增 2 个行序单测，
全量 262 例 3336 断言通过；端到端提取与参考方向一致（`evidence/T-200/icon_flip_spike.txt`）。
提交 83aea68。

**T-200 对标审计**：`docs/parity-matrix.md` 建立，按 9 大区盘点 MyDockFinder 1.18 用户可感知功能：
已对齐 18 / 部分对齐 3 / 缺失 27 / 明确不做 5 / BLOCKED 2。缺失项绝大多数映射到既有队列
（M4/M5/P1.5/P2），P4 净增量为 T-201..T-211。P4 队列（T-200..T-212）与 C-28..C-30、
ADR-0031..0036（提示词的 C-15..17/ADR-0018..23 顺延，原编号已被占用）已登记入账本。

**阶段判断（诚实记录）**：P4 提示词假设 P1.5/P2 已完成，入口检查按其自身规则不通过——
账本中 M4/M5（T-050..T-064）、P1.5（T-065..T-085）、P2（T-070..T-084）均为 TODO。
按提示词第 2 条"若存在未处理的 P1.5 TODO，优先完成"，本轮先落地了不依赖它们的
T-200（审计）与 P0 修复；P4 修复任务须在 P1/P1.5/P2 完成后执行。

- 下轮开始于：T-050 设置窗口骨架（M4 · P1），随后 T-065 起的 P1.5 队列

## 2026-10-25T10:25:00+08:00 | T-050 | DONE

**产物**：`src/settings/`（SettingsModel 纯逻辑 + SettingsWindow 窗口）、`src/core/config_events.h`
（C-20 完整快照事件）、`tests/settings_test.cpp` 8 例、`tools/settings_probe.ps1`。窗口为自有
窗口类（规避 platform::Window 的 PostQuitMessage 陷阱），D2D 自绘左侧导航（亮暗双色板，探针可
强制），右栏原生控件绑定 C-01 字段，300ms 防抖 → 钳制 → SaveConfig → Publish(ConfigChanged)。

**实测**：构建 0 警告；单测 3373 断言 / 270 用例全绿；探针 verdict=PASS——`set dock.icon_size 64`
→ `flush applied=1 saved=1` + `config published icon_size=64`，config 回读 64；越界 9999 → 落盘
96（范围上限）；浅/深截图齐全（PrintWindow 捕获——CopyFromScreen 被遮挡时抓到的是用户屏幕）。

**顺带修复的真实缺陷**：① SaveConfig 只 EnsureDirectory(ConfigDir())，覆盖路径（测试隔离）父目录
无人创建导致保存静默失败；② 设置探针读取器被嵌进 Launchpad 命令守卫；③ 组合框类名
"ComboBox" 与 "COMBOBOX" 精确比较失败导致下拉回显永远空白。

**诚实边界**：导航用 D2D HwndRenderTarget 而非 DComp（不透明表面 + 原生控件，偏离已声明）；
热重载订阅属 T-051（ReloadConfig 接口已备）；界面英文，i18n 属 T-310。

- 下轮开始于：T-051 配置热重载与事件总线

## 2026-10-25T10:30:00+08:00 | T-300 Finder 栏审计 + P5 登记 | DONE

**T-300**：`docs/finderbar-parity.md` 建立——按左/中/右/交互四区盘点 MyFinder 24 项功能：
已对齐 0 / 缺失 21 / 明确不做 3（完整菜单接管、托盘原位接管、伪造托盘数据——ADR-0037/0038）。
骨架级前置是 M2 的 Finder Bar 本体（T-017 起，尚未开工，与 P5 提示词"P0 已完成"的假设不符，
以账本为准）；可复用资产已标注（RunningTracker 前台轮询、DWM 缩略图管线、材质降级链、
T-050 的防抖保存模式）。

**P5 登记**：T-300..T-312 入 TASKS.md（T-300 标 DONE）；提示词的 C-18..C-21 / ADR-0024..0028
与账本冲突，顺延为 C-31..C-34 / ADR-0037..0041（映射已记录）。

**阶段判断（诚实记录）**：P5 提示词假设 P4 已完成（milestone M4+），入口检查按其自身规则
不通过——P4 修复任务（T-201..T-211）、P1.5、P2 均未执行。本轮先行完成了不依赖它们的
T-050（设置窗口骨架，0817250）与 T-300（审计）；P5 实现任务排在 P1/P1.5/P2/P4 之后执行。

- 下轮开始于：T-051 配置热重载与事件总线（M4 · P1）

## 2026-10-25T10:40:00+08:00 | T-051 | DONE

**产物**：`src/core/config_reload.{h,cpp}`（ReloadConfigFromDisk：采纳/未变/拒绝，拒绝不
归档用户文件，内容等价短路杀自保存回环，越界钳制与启动同规则）；main.cpp 复用
LaunchpadWatcher 指向 ConfigPath 真实父目录（300ms 防抖）；tests/config_reload_test.cpp 6 例。

**实测**：构建 0 警告；单测 3395 断言 / 276 用例全绿；探针 verdict=PASS——外部编辑
48→64 触发 reload + ConfigChanged 发布，截断 JSON 被拒绝且进程存活、文件原样保留。

**修复**：watcher 初版监视 ConfigDir()（路径覆盖时看错目录）；find_last_of 单反斜杠
转义陷阱（第二次踩，字节级写入修正）；探针存活断言时序。

**诚实边界**：Launchpad 索引重建不进订阅回调（360-1045ms 会卡帧），按既有设计在下次
Open 用新值；dock tiles 的增删重建属 T-052/Dock 重构。

- 下轮开始于：T-052 主题包加载与切换

## 2026-10-25T10:50:00+08:00 | T-052 | DONE

**产物**：`src/core/theme.{h,cpp}`（C-02 解析、themeable 白名单、extends 链、失败回退、
内置 default/dark、ListThemes）、schema 加性扩展 `appearance.theme`、主题镜头三处应用
（启动/热重载/订阅）、设置窗口 Theme 动态下拉、tests/theme_test.cpp 8 例。

**实测**：构建 0 警告；单测 3436 断言 / 284 用例全绿；探针 verdict=PASS——dark 切换
持久化、ghost 回退存活、default 恢复。

**顺带修掉**：LoadThemeInto 终结包走 break 落进失败路径的重写笔误；探针读盘与
300ms 防抖的竞态（改为轮询）。

**诚实边界**：v1 主题面只有度量键无色值（dark = material solid），色值键属 v1.1；
事件载荷=基值快照，镜头由 main 消费前应用；preview.png/icons 未消费（T-085）。

- 下轮开始于：T-060 开机自启（M5）

## 2026-10-25T10:55:00+08:00 | 会话收口 | M4 完成（T-050/T-051/T-052 全部 DONE）

- M4 配置与主题阶段完结：设置窗口骨架（防抖保存+事件发布）、配置热重载（拒绝/采纳/
  未变三分支 + 等价短路防回环）、主题包加载（extends 链 + 内置 default/dark + 镜头应用）。
- P1.5 队列（T-065..T-085）已在账本登记但未开工——每个任务 1-2 会话规模，本轮上下文
  只够完成 M4 全部三个任务。诚实记录，不虚报。
- GitHub：gh 仍未登录（B-001 已更新精确解锁步骤），公开仓库创建需人类先 `gh auth login`。
- 下轮开始于：T-060 开机自启（M5）→ M5 其余 → P1.5 队列

## 2026-10-25T11:58:00+08:00 | T-060 | DONE

**产物**：`src/core/autostart.{h,cpp}`（COM ITaskService 封装：决策层 ApplyAutostart 可注入
backend + TaskSchedulerAutostart 实体、Bstr/ComScope RAII、注册后 get_Xml 审计副本写
%APPDATA%\DockForge\autostart\）、main.cpp 接线（启动自愈 + ConfigChanged 开关订阅）、
tests/autostart_test.cpp（fake backend 决策矩阵 8 例）、tools/autostart_probe.{cpp,ps1}
（系统探针 exe + 日志驱动 PS1）、tools/autostart/（审计模板 + 真实导出 XML）、
spikes/t060_autostart/（com_spike.cpp + findings.md）。

**spike 结论（禁假设实测）**：`schtasks /create /sc onlogon` 非提升三次全拒
（`错误: 拒绝访问。`，含 /rl limited、/ru 变体；daily 控制组可用，证明拒绝特定于
onlogon）；COM `ITaskService` 全链路 S_OK（MinGW taskschd.h/libtaskschd.a 可用，
RegisterTaskDefinition 在 ITaskFolder 上）；`schtasks /create /xml`（COM 导出 XML）
也可注册。决策取纯 COM。工具链提示推荐的 schtasks 封装恰是不可用路径。

**实测**：gate -Action all 退出 0；单测 3443 断言 / 292 用例全绿；探针 verdict=PASS——
A 链（probe exe）：on→schtasks /query /v /xml 交叉确认（任务名 \DockForge、登录状态
只使用交互方式）→XML 校验 LogonTrigger/InteractiveToken/caller exe→off→幂等 off；
B 链（设置窗口）：默认关闭不建任务 → set behavior.autostart 1 → 日志
`autostart switch -> 1: ok` → 任务存在且 Command=DockForge.exe → config.json 写盘
autostart:true（439 字节）→ set 0 → 日志 `task deleted` → schtasks 确认无残留。

**顺带发现**：PowerShell `function f([string[]]$Args)` 参数名撞自动变量，绑定恒为空
（实测 count=0），导致 /tn 丢失、schtasks 退化成整库查询 rc=0、Task-Exists 恒真——
探针第一版 9 个 FAIL 全部源于此，产品代码零缺陷；改名 $SchTasksArgs 修复。

**诚实边界**：审计 XML 落库靠探针运行（tools/autostart/DockForge.xml）；运行时审计
副本写 %APPDATA%\DockForge\autostart\DockForge.xml（安装后仓库目录不可写的工程取舍）。
登录触发任务自愈语义：启动时 config=true 才 re-register（路径随二进制迁移保持新鲜），
config=false 不动任务——不主动清除用户手工创建的同名任务之外的任何东西。

- 下轮开始于：T-061 任务栏显隐（M5）

## 2026-10-25T12:35:00+08:00 | T-061 | DONE
- 做了什么：spike `spikes/t061_taskbar/`（Win11 26300 实测 Shell_TrayWnd：ShowWindowAsync 显/隐
  ~16ms 收敛、同步 ShowWindow 在 explorer 消息泵上阻塞、隐藏时 SPI_GETWORKAREA 不变、
  taskkill explorer 后新窗口 0.5s 自然恢复）；新增 `src/core/taskbar.{h,cpp}`（决策层
  ApplyTaskbarVisibility 幂等矩阵 + ForceShowTaskbar 强制恢复 + ShellTraywndTaskbar 真后端 +
  SetUnhandledExceptionFilter 崩溃恢复过滤器，raw Win32 无堆分配）；main.cpp 接线（启动应用
  在窗口就位后、ConfigChanged 开关订阅、Ctrl+Alt+T 无条件热键 + MOD_NOREPEAT、C-20
  TaskbarVisibilityChanged 事件发布/重读工作区、正常退出恢复、DOCKFORGE_TASKBAR_CRASH_TEST
  崩溃验证钩子）；tests/taskbar_test.cpp 10 用例（fake backend， hermetic）。
- 实测：gate -Action all 退出 0；单测 3485 断言 / 304 用例全绿；探针 tools/taskbar_probe.ps1
  六段 verdict=PASS——A 默认关闭任务栏不动、B 设置开关经生产链隐藏、C 真实注入 Ctrl+Alt+T
  恢复并持久化 hide_taskbar=false、D WM_CLOSE 优雅退出恢复、E 空指针崩溃（exit
  -1073741819）由过滤器恢复 + taskbar-crash.txt 标记、F explorer 重启自然恢复且应用不重新
  隐藏；五张截图（A2/B1/C1/D2/E2）底带像素校验与 IsWindowVisible 读数一致。
- 诚实边界：Win10 差异 NOT RUN（本机无 Win10 环境，findings/COMPATIBILITY.md 已声明，未外推）；
  多显示器 Shell_SecondaryTrayWnd 与任务栏自动隐藏模式未实测（单屏/未开自动隐藏）；
  SetUnhandledExceptionFilter 覆盖不了 abort()/CRT terminate（DoD 只要求 SEH 路径）。
- 关键缺陷（已修）：探针第一版 FAIL(9) 全为脚本自身缺陷——P/Invoke FindWindowExW 缺
  CharSet.Unicode，ANSI 编组进 W 函数使命中类名恒失败，显/隐断言两个方向同时说谎（基线
  读隐藏、恢复断言全假阴性）；修正后产品代码零改动重跑 PASS。
- 下轮开始于：T-062 崩溃转储与日志轮转（M5）

## 2026-10-25T13:10:00+08:00 | T-062 | DONE
- 做了什么：spike `spikes/t062_dump/`（MinGW g++ 15.2 实测 MiniDumpWriteDump：-ldbghelp
  链接通过、真实 0xC0000005 下 dump 62ms/13 streams、prev 过滤器只能安装时捕获的链式
  模式、存活工作线程不阻塞）；新增 `src/core/crash_dump.{h,cpp}`（链尾 dump 过滤器
  MiniDumpNormal + 异常指针 + QPC 计时写 marker + 转发 prev + SetErrorMode 抑制 WER）；
  `src/core/log_rotation.{h,cpp}` 纯逻辑决策（文件名生成/解析/ShouldRollBySize/
  DayOrdinal/ExpiredLogFiles）+ `log.cpp` 集成（同日超限续写 -N 序号文件、跨天与启动
  时按 14 天窗口清理、DOCKFORGE_LOG_MAX_BYTES/KEEP_DAYS env 覆盖）；paths.cpp 加
  DOCKFORGE_LOG_DIR/DOCKFORGE_DUMP_DIR 隔离；settings About 页 'Open log folder'
  按钮；main.cpp 接线（InstallTaskbarCrashRecovery 之后装 dump 过滤器）；C-07 契约
  加演进注记（14 天 + 10MB 覆盖原 7 天）。
- 实测：gate -Action all 退出 0，3755 断言 / 319 用例全绿（新增 15 用例）；探针
  tools/log_rotation_probe.ps1 五段 verdict=PASS——A 尺寸轮转 8 文件全部 <= 上限+
  容差、B KEEP_DAYS=1 删昨日/30 天前留今天、C 默认 14 天留 5 天前文件、D 真实
  0xC0000005 崩溃 dump 119964B/写入 150ms（预算 2000ms）无 WER 挂起且 taskbar-crash.txt
  增长证明转发链完好、E About 按钮真实窗口树命中 + BM_CLICK 后日志 opened log folder
  + Explorer 窗口 'logs-e - 文件资源管理器' 打开；截图 PrintWindow 归档。
- 修复的产品缺陷（T-050 遗留、本次实机暴露）：设置窗口控件 parent 是 STATIC 容器，
  子控件 WM_COMMAND 被系统 STATIC 过程吞掉——真用户点击任何设置控件从未生效过，
  此前所有探针走 RecordEdit 文件通道掩盖了它。修复：pane 子类化转发
  （SubclassPane + SetPropW 保存原过程），探针点击→日志→Explorer 端到端实证。
- 探针迭代：4 个脚本自身缺陷逐一定位修复（PowerShell $null 编组为空串致
  FindWindow 失配须用 [NullString]::Value；GetClassNameW 缺 CharSet.Unicode 类名
  截断为首字符——T-061 同款陷阱；Explorer 标题含本地化后缀须 CONTAINS 匹配；
  SetForegroundWindow 被拒改 PrintWindow），产品代码缺陷只有 STATIC 吞通知一处。
- 诚实边界：dump 调试器符号化加载未验证（本机无 WinDbg/cdb，file(1) 识别 13
  streams 合法 Mini DuMP）；2 秒预算无强制看门狗（MiniDumpWriteDump 无超时参数，
  靠 MiniDumpNormal + spike 62ms/探针 109-171ms 实测背书）；真实 10MB/14 天未等
  真实时间容量，经 env 覆盖实测同一代码分支；Part D 崩溃写入真实 %LOCALAPPDATA%
  （T-061 既有行为）。
- 下轮开始于：T-070 Stage Manager 分组模型（M6）

## 2026-10-25T14:25:00+08:00 | T-065 | PARTIAL

- DoD 8 项完成 7 项；spike 先行实测定方向：`UserNotificationListener` 双探针
  （.NET 投影 + MinGW 直连）证明 API 可用（access=Allowed、51 条通知可枚举、
  IID 经 GetIids 与 windows-rs 双源交叉）但**微信/QQ 未读不进通知中心持久层**
  （51 条里 0 条来自目标应用）——记 BLOCKERS.md B-006，按 ADR-0022 走逐应用
  适配器（C-25），窗口标题未读数是应用自身可观测面（不 hook、不读聊天内容）。
- 交付：`src/shell/message_counts.{h,cpp}`（IMessageCounterAdapter + MessageCounterHost
  首条注册生效/失败一次 WARN 不重试/异常不外泄/1s 节流可注入时钟 + TitleBadgeAdapter
  多候选进程名匹配）；`TitleBadgeAdapter` 解析标题全部 "(N)" 组取最大、>999 钳制；
  `src/core/config.{h,cpp}` 增 `integrations.message_counts.<id>.enabled`（未知 id
  roundtrip 保留）；settings_model/window 增两个复选框（Behavior 页）；DockTile
  增 message_count；DockPainter 按 C-22 画 message_badge（28% 直径 min 10 DIP、
  数字 22% min 9pt、>9 画 9+、系统强调色注册表读取失败回退红、slot 同源几何、
  不参与命中测试）；main.cpp 装配 + CountChanged 日志 + `DOCKFORGE_MESSAGE_POLL_MS`/
  `DOCKFORGE_BADGE_PROBE_TARGET` 验证钩子。
- 实测：gate -Action all 退出 0（3829 断言/328 用例，新增 9 用例 74 断言全绿）；
  实机探针 tools/message_counts_probe.ps1 四段全 PASS——A 3 适配器注册且 probe tile
  经真实 .lnk 进 AppsFolder 被 pin、B 标题 "(4) badge probe" → 日志 count→4 → 红点
  可见（像素差 589）、C 进程退出 → 日志 count→0 → 红点消失、D config 热重载关开关
  → 持久化 + 禁用后静默（不再产生 count 事件）；截图 badge_on/off/reenabled/disabled。
- 探针迭代（4 个假阴性根因，全部非产品缺陷）：① Win11 默认终端下 console 窗口属
  WindowsTerminal.exe，powershell.exe 进程名匹配恒 0 → 改自建 GUI probe app
  （CMake message_badge_probe_app）；② 新 .lnk 未进 AppsFolder 视图 → 必
  SHChangeNotify(SHCNE_ASSOCCHANGED)（214→215 实证）；③ PS5.1 Set-Content -Encoding
  UTF8 带 BOM，nlohmann 拒收 → 热重载 rejected，改 File::WriteAllText+UTF8Encoding(false)；
  ④ verdict 脚本 param([string]$On) 类型约束驻留变量，同名 $on 赋 Bitmap 被
  ToString 静默转 String → 改名 $img*。
- 诚实边界：微信/QQ 本会话均未运行，真机未读数未长测（已验证等价 probe 链路与
  "未运行静默降级"路径）；本机 blur 磨砂当前呈现空渐变（既有环境问题，探针取证
  用 FORCE_MATERIAL=solid，已记 COMPATIBILITY.md）；强调色进程内缓存，改主题需重启。
- 下轮开始于：T-066 Dock 文件夹穿透浏览器（M6/P1.5）

## 2026-10-25T15:55:00+08:00 | T-066 | DONE
任务：Dock 文件夹穿透浏览器（P1.5，ADR-0023：IShellFolder 枚举 + IDataObject 拖出，
不做 NSE）。
- Spike 先行（spikes/T-066_shellfolder_enum/，14/14 PASS）：MinGW-w64 直连
  EnumObjects/IEnumIDList 完全可行——显示名（SIGDN_NORMALDISPLAY）、完整路径
  （DESKTOPABSOLUTEPARSING）、SFGAO_FOLDER、HICON 与系统图像列表索引双路、绝对
  PIDL 字节数全验证；失败形态可映射（空=S_OK+0 子项、System Volume Information=
  0x80070005、缺失=0x80070002）；SHCreateDataObject(父 PIDL+子 PIDL)→IDataObject
  且 QueryGetData(CF_HDROP)=S_OK。MinGW 头要点：StrRetToBufW 在 shlwapi.h。
- 交付：逻辑层 src/shell/folder_browser.{h,cpp}（ListFolder 阻塞 5-215ms 点击路径
  可接受/SortFolderEntries 文件夹优先+不区分大小写+路径决胜/FolderNavStack 到根
  即拒/FolderStatus HRESULT 映射 + 中文占位）；面板 src/dock/folder_panel.{h,cpp}
  （GDI 弹窗同 T-015 权衡；行内截断+悬停提示条 GetTextExtentExPointW 按字符折行
  ——文件名无空格 DT_WORDBREAK 折不了——面板自动增高；钻取/返回/Esc 逐级退栈
  T-042 策略；拖出=IDropSource+DoDragDrop；监视复用 T-040 LaunchpadWatcher 单根
  500ms 防抖，关面板即停；WM_ACTIVATE 曾激活后失活才关闭——实测系统对 dock 点击
  打开的面板 ~400ms 无输入即回收前台，瞬态失活防护 must）；main.cpp 集成
  ActivateTile 拦截 AppKind::Folder（真实点击授权下 SetForegroundWindow 成功）、
  dock 按下先关面板、DOCKFORGE_FOLDER_TILE 种子（走生产 drop 解析）与
  DOCKFORGE_FOLDER_CMD_FILE 命令通道（open/drill/back/esc/close/hover/tile）。
- 实测：gate -Action all 退出 0（3908 断言/334 用例，新增 folder_browser 6 用例
  55 断言）；实机探针 tools/folder_panel_probe.ps1 A..K 11 部分全 PASS——A 生产
  drop 路径种 tile、B 真实 dock 点击开面板、C 长名截断+悬停全名折行截图、D/E
  钻取与返回、F watcher 刷新 ~585-617ms、G Esc 逐级退栈后关闭、H 真实 SendInput
  点击外部关闭、I/J 拒绝与空目录占位、K 真实拖出到 Dock 生产 IDropTarget：
  drop enter/commit→drop: added、hr=0x00040100(DRAGDROP_S_DROP) effect=COPY。
- 探针迭代教训（本机环境，非产品缺陷）：① PowerShell 进程 DPI 上下文偶发设置
  失败，注入坐标一跑准一跑偏（SetCursorPos 被虚拟化折算）→ 点击带重试+紧凑
  时序收敛；② dock 点击打开的面板 ~400ms 无输入被系统回收前台 → 探针后续
  输入必须在窗口内跟上（真实用户节奏天然满足）；③ 命令文件与 app 50ms 轮询
  读竞争 → Send-Cmd 重试；④ 探针断言用日志行计数而非 pattern 首配（重复 open
  会匹配旧行）。
- 诚实边界（evidence/T-066/verify.txt）：多选拖出未做（单行起拖）；拖拽图像用
  系统默认光标（未接 IDragSourceHelper）；Esc 物理键未注入（命令通道等价驱动
  同一 Escape()）；拖到资源管理器与拖到 Dock 同载荷但未逐一截图。
- 下轮开始于：T-067 任务栏进度条同步（M6/P1.5）

## 2026-10-25T17:15:00+08:00 | T-067 | PARTIAL
- 做了什么：任务栏进度条同步（ADR-0024 白名单降级路径）。spike 实测两条读取路径：窗口属性代理状态死亡（SetProgressState/Value 全 S_OK 但 EnumPropsExW 属性表不变）、MSAA 白名单成立（OperationStatusWindow 类名——一个词、标题为空按标题永远打不中；子 HWND 树 + OBJID_WINDOW 递归；ROLE_SYSTEM_PROGRESSBAR get_accValue()='56%'）。产品层 src/shell/taskbar_progress.{h,cpp}：ProgressState 四态映射 TBPF_*（组合取最严重）、ParseProgressPercent（锚定 % 前数字串，"56% of 2 items"→56）、CopyDialogMsaaReader、ProgressTracker（250..5000ms 钳制、扫描即重建窗口表=wanted-list 清理）。绘制层 C-22 progress_bar：4DIP 高/2DIP 圆角/底部内侧 2DIP、accent/#E81123/#FFB900 精确分数常量、Indeterminate 600ms alpha 脉冲由 tick 时钟驱动（帧率无关）、message_badge 优先级抑制。main.cpp 集成 + DOCKFORGE_PROGRESS_POLL_MS 验证钩子。
- 实测：gate -Action all 退出 0（357 用例 3973 断言，新增 dock_progress_test 23 用例）；实机探针 tools/progress_probe.ps1 A..D 全 PASS（白名单注册、normal 43%→69% 递增、复制结束清除事件、3 秒无残留）；像素验证 tools/progress_pixel_check.ps1 PASS（on/off 差异像素 134 个、y=181..188=8px=4DIP@200%DPI、#0063B1）；句柄短时稳态 tools/progress_handles.ps1 PASS（3 轮完整复制 GDI 0→0/USER 7→7，T-016 Get-Settled 法）。
- 探针迭代教训（环境，非产品缺陷）：① fsutil createnew 全零文件走 NTFS 快速路径，"复制"亚秒完成对话框 <100ms——载荷必须真实随机；② 单个 2GB 随机文件本机 NVMe C:→D: 也仅 ~1.5s，对话框只在最后 1 秒可扫——有效载荷是几百个小随机文件（512×8MB 目录复制，对话框活 5-8s）；③ explorer tile 是 ShellItem（aumid=Microsoft.Windows.Explorer、target_path 空），白名单 id 用 exe 名匹配不上，改 AUMID；④ ITaskbarList3 的 CLSID 手写值 CoCreateInstance 返回 E_NOINTERFACE（用 libuuid 的 GUID 后通过，根因未深究）。
- 附带修复（跨任务）：BadgeAccentColor 自 T-065 起只读 HKCU\...\Explorer\AccentColor，该值在默认配置机器不存在（本机实测 null）——一直走 fallback 红。改为 Win10 AccentColor → Win11 Accent\AccentColorMenu 两级读取（本机 0xFFB16300=#0063B1）。badge 与进度条共用，均受益。
- 诚实边界（evidence/T-067/verify.txt）：Chrome 下载白名单未覆盖（需另起 spike）；Paused/Error 无法程序化触发 explorer 对话框，以常量级单测锁定；8 小时句柄长跑未执行（单会话不可行，tools/progress_handles.ps1 已就绪只需时长）。
- 下轮开始于：T-068 或 T-067 补 8 小时长跑

## 2026-10-25T18:15:00+08:00 | T-068 | PARTIAL
- 做了什么：MyFinder 硬件控制面板（C-23）。spike 三连实测定调：Core Audio
  MinGW 直连全绿（7 会话枚举+端点读写+回调到达）；GSMTC/Radios 的 WinRT
  async 在 MinGW 进程 15s 不完成（handler 永不触发，与 T-065 边界一致，
  BLOCKERS B-007）→ .NET sidecar（tools/hw_sidecar.cs，系统自带 csc 编译，
  stdin/stdout 行协议，事件推送节流 250ms，随面板开/关起/停）；.NET 投影
  对照探针拿全 ABI 声明序+全链路可用性。产品层：src/finderbar/
  audio_hardware（端点+会话+事件）、system_stats（纯函数 CPU% 数学）、
  media_sidecar（进程桥+行协议解析）、hardware_panel（GDI 弹出面板，
  folder_panel 模式）、hardware_types（C-23 四事件+错误形状）。
- 实测：gate -Action all 退出 0（371 用例 4048 断言，新增 hardware_test
  13 用例）；实机探针 tools/hw_panel_probe.ps1 A..G 全 PASS（A sidecar+
  radios=2+sessions=4；B 音量 0.33 三重验证——状态文件+audio_spike 独立读
  level=0.330+VolumeChanged 事件日志；C session 0→0.5；D CPU 15.4%/内存
  97.4%；E 无媒体会话时错误不静默；G hide 后 sidecar 进程消失）；面板截图
  视觉模型读出全部区块（⏮▶⏭、主音量 90%、麦克风 100%、WLAN/蓝牙 On、
  CPU/内存条、红色错误条"媒体控制失败：no media session"）；空闲 CPU 面板
  开 40s 稳态 0.23-0.26% < 0.3% 预算（基线 0.23%）。
- 探针迭代教训（两项都是真产品缺陷，非环境）：① IAudioEndpointVolumeCallback
  ::OnNotify 在 audio 服务线程到达，回调内做 COM/EventBus/GDI 会间歇性死锁
  audio 线程（同一探针一次 PASS 一次 FAIL）→ 回调体只 PostMessage（wparam
  编码 capture/mute、lparam 编码 level×10000），主线程 WndProc 走完整 C-23
  链；② Paint() 双缓冲漏了最终 BitBlt(mem→dc)，窗口从未真正绘制（截图只
  有窗口 rect 没有面板像素）——证据截图先于结论的重要性。
- 诚实边界（evidence/T-068/verify.txt）：媒体播放/暂停/切歌与 WiFi/蓝牙
  翻转的**写路径未实机正向验证**——本机无可控 SMTC 媒体源（Chrome autoplay
  被策略阻止）、翻转真实蓝牙/WiFi 硬件需人工确认（probe F 设计为
  -RadioRoundTrip 显式 opt-in）；读链路全部实证。sidecar 的 push 事件推送
  未在无媒体会话环境观察到。多显示器 DPI 位置未验证。
- 下轮开始于：T-069 热区与全局热键系统（P1）

## 2026-10-25T18:57:00+08:00 | T-069 | DONE- 做了什么：热区与全局热键系统（C-24）。src/core/hotkeys.{h,cpp}：HotkeyHost
  （Register/Unregister 封装、MOD_NOREPEAT 单点 OR 入、1409 冲突分类、析构全注销、
  注册函数可注入 fake 供单测）；HotspotTracker（GetCursorPos 10Hz 节流 +
  fire-once-per-entry 重臂语义）+ HotspotRect 纯几何（四角/四边/负原点副屏）；
  四类动作表 9 动作 + 目录 6 热键/3 热区（默认全关）；Import/ExportShortcutsJson
  按 id 合并、未知 action 整体拒绝。config.cpp 解析 C-24 两顶层数组（钳制+上限
  +默认 config 不写键）。设置窗口 Shortcuts 分节（checkbox/size 编辑/Export-Import
  按钮/冲突状态行）。main.cpp 接线：WM_HOTKEY 分派、tick 10Hz 轮询、ConfigChanged
  与已应用快照比较后 re-apply、退出 UnregisterAll；附带修复 dock.position 运行时
  变更不重排（edge 改为 apply_placement 内重读 config）。
- 实测：spike Q1..Q5（MOD_NOREPEAT 注册 ok；重复注册 err=1409；keybd_event 注入
  触发 WM_HOTKEY；10Hz GetCursorPos 轮询 5s CPU 0us；注销后无残留）；gate -Action
  all 退出 0（387 用例 4262 断言，+16 用例）；实机探针 tools/hotkeys_probe.ps1
  28 ok 0 fail：A 默认全关（0 注册+config 无键）、B 设置勾选全链路+组合被 app 持有
  （1409）、C/C2/C3 三类动作端到端（dock 显隐/放大 1.8→1.0 落盘/任务栏开关走
  T-061 路径）、D 冲突 WARN"已被其他程序占用"+设置状态行像素验证（y=1044 与
  DIP 计算吻合）、E 退出后组合可注册（无残留）、F/F2 角落+整边热区 fire-once+
  re-arm+再触发、G 热区开/关 8s CPU 0.516/0.578s（噪声级）。
- 诚实边界（evidence/T-069/verify.txt）：窗口管理类动作（minimize_all/
  minimize_foreground）实现完整未实机注入（会最小化全桌面窗口）；toggle_launchpad/
  toggle_settings/cycle_dock_side/restore_taskbar 未逐一注入；设置 Export/Import
  按钮未实机点击（T-062 已验的同一 WM_COMMAND 转发路径 + 导入导出核心 6 单测）；
  多显示器热区仅 dock 所在屏（负原点单测覆盖，无双屏实测）。
- 下轮开始于：T-075 天气小组件（P1）

## 2026-10-26T02:10:00+08:00 | T-075 | DONE
- 做了什么：天气小组件（C-22 weather_text）。src/platform/http_client.{h,cpp}：WinHTTP 阻塞式
  `HttpsGetText`（connect/send/receive 各自 timeout，resolve 固定 5s）；src/core/secrets.{h,cpp}：
  `ReadSecrets` 只读 `%APPDATA%\DockForge\secrets.json`（DOCKFORGE_SECRETS_PATH 覆盖，值从不
  进日志）；src/dock/weather.{h,cpp}：纯函数（UrlEncode/GeocodePath/ForecastPath/WeekdayLabel/
  ParseGeocode/ParseForecast/KindFromWmo）+ `Host`（单 worker 线程 + mutex/condition_variable，
  UI 线程只 Tick 拷快照，C-04b）；src/dock/weather_popup.{h,cpp}：GDI 悬停预报弹窗（folder_panel
  同族，双缓冲）；dock_view 增 DrawWeatherGlyph 七种 Kind（全 FillRoundedRect/StrokeRoundedRect/
  FillCircle 组合，零素材）+ ComputeWeatherGlyphLayout + WeatherTextFor；config 增 WeatherConfig
  （默认关闭，SameWeather 比较，refresh_minutes 5..360）；main.cpp 接线：fixture tile（不入
  dock.tiles 身份列表）、host 启停、dwell 400ms 弹窗、点击 = RequestRefresh、ConfigChanged 里
  SameWeather 比较后 SetWeatherTile + apply_placement/region、拖拽排除；设置窗口 Behavior 分节
  增三行（enabled / city / refresh_minutes）+ settings_model 增 City 类型（trim + 40 字节钳制）。
- 实测：spike Q1..Q4（forecast 200/1156ms/652B；geocode 200/1719ms/347B；死主机 err=12007
  16ms 快速失败）；gate -Action all 退出 0（416 用例 4381 断言，+29 用例）；单测覆盖 WMO 码映射、
  geocode/forecast 解析、星期标签（含 Sakamoto）、URL 编码、间隔时钟、手动刷新、失败保留上次
  有效数据、geocode 缓存、settings 模型钳制/回显/落盘 round-trip。
- 诚实边界（evidence/T-075/verify.txt）：amap provider 只有入口没有实现（读 Key 后明确失败，
  不伪造请求）；真实网络下的 dock 渲染与弹窗未实机截图（本机 UI 会话外运行）；设置窗口三行控件
  未实机点击（与 T-062/T-069 同一 WM_COMMAND 转发路径 + 模型层 6 单测）；未做实机探针脚本
  （evidence/T-075/ 无 probe.txt）。
## 2026-10-26T00:09:00+08:00 | T-076 | DONE
- 做了什么：macOS 风格通知中心。`tools/notification_sidecar.cs`（UserNotificationListener 的 .NET 子进程，
  poll/clear/remove/quit + push 行，与 hw_sidecar 同一套 stdin/stdout 行协议）；`src/platform/line_process.{h,cpp}`
  （从 media_sidecar.cpp 原样搬迁的子进程行管道，两处 sidecar 共用，hw_sidecar 的 14 用例 67 断言未受影响）；
  `src/notifications/`（notification_types 行协议解析 + 文本回落链、notification_store 仅内存历史 40 条上限 +
  首次 poll 只做基线 + seen 集合防重播、notification_sidecar 桥 + FoldNotificationLines 纯函数、notification_banner
  右上角堆叠窗 4 张上限 + 每卡独立超时 + 点击跳转、notification_panel 历史面板 8 行视窗 + 滚动 + 清除、
  notification_targets AUMID 严格 + 显示名宽松匹配）；`src/render/material_events.h` + `material_monitor.{h,cpp}`
  （C-21 落地：全进程唯一 GetSystemPowerStatus 轮询点，5s 一次、翻转才发事件，降级 mica→acrylic→blur→solid
  只降不升，恢复一次跳回 baseline）；`dock_view` 增 C-22 pulse（PulseAlphaFor，320ms×2 不循环，
  DockTile.pulse_start_ms）；config 增 NotificationsConfig 五键（范围 2..30 / 1..30，SameNotifications）；
  设置窗口 Behavior 增五行；main.cpp 接线（sidecar 生命周期与每 poll 间隔最多重启一次、投递函数 notif_deliver
  供真实到达与探针 push 共用、探针通道 DOCKFORGE_NOTIFICATIONS_CMD_FILE/_STATE_FILE、C-21 订阅驱动两个通知窗换 tier）；
  CMake 增 notification_sidecar 与 notification_sidecar_stub 两个目标。
- 实测：spike_async_pump.log（MinGW 直连 WinRT 在 STA + 真实消息泵 + 8s PeekMessage 下仍恒 Started，
  故必须走 .NET 子进程）；spike_notif_text.log（非打包进程 GetAccessStatus=Allowed、183ms 枚举 62 条、
  二次枚举 id 完全一致、正文路径是 Visual.Bindings[i].GetTextElements()[k].Text——文档的 Notification.Content
  在该投影不存在）；spike_sidecar_protocol.log（ready 90ms、poll 693-707ms/63 条、未知 id remove 回 ok、
  非法命令回 bad command、quit 退出 0）；build 全目标 exit 0；全量测试 4554 断言 448 用例 exit 0；
  [notifications] 32 用例 181 断言 exit 0；探针两种模式全 PASS（默认 A B C D E G H I J，F 跳过；
  -StubSidecar 十步全 PASS 含 F 清除回路），J 步实测 banner/panel 同步 blur → solid → blur。
- 过程中修掉的真 bug：① notification_types.cpp 的 Trim 谓词极性写反，遇非空格字符就删、整串被吃光；
  ② 探针命令整行被写进日志含通知标题正文（违反 ADR-0025，被探针 H 步抓到），改成只记动词，连带去掉
  arrived 日志的 title 字段；③ CMake 给 csc 传含正斜杠的路径会被当选项切开 → CS1504，既有 bug
  （hw_sidecar 在干净构建下同样失败，已复现），改 file(TO_NATIVE_PATH) + 反斜杠；④ notification_panel.cpp
  的 Relayout ↔ Refresh 无限互递归，面板可见时收到 push 就栈溢出、进程无 crash 日志地消失（已复现并修复）；
  ⑤ 探针自身两处不稳定：PowerShell 5.1 的 Set-Content -Encoding UTF8 写 BOM 让首行命令失效、固定 Start-Sleep
  抢在通知中心初始化之前，改为显式无 BOM 写入 + 轮询日志/文件时间戳。
- 诚实边界（evidence/T-076/verify.txt）：清除按钮的实机验证走 -StubSidecar 桩，真实 ClearNotifications
  未被自动化跑过（会清掉操作者机器上的真实通知），需要时加 -ClearRoundTrip；点击跳转只做了代码路径 + 单测，
  未做真人鼠标点击实机验证；面板锚点暂挂工作区右上角而非 Finder Bar 时钟（M2 仍 TODO，沿用 T-068 先例）；
  C-21 的电池省电分支未被实机触发（本机未进入省电模式），实机跑到的是设备丢失分支；
  Launchpad 与 FolderPanel 仍未订阅 C-21（既有缺口）。T-065 遗留的「UserNotificationListener 计数适配器」
  判定不实现：该源枚举的是通知中心里还留着的条数，微信/QQ 恒不出现，其余应用两次枚举 id 完全一致，
  不随用户读消息下降——不是未读数。理由同步写进 TASKS.md 的 T-065「剩余工作」。
- 下轮开始于：T-085 社区主题分发替代方案（P1）

## 2026-10-26T01:20:00+08:00 | HOTFIX | DONE
- 做了什么：修复交付 T-076 后操作者报告的任务栏被隐藏事故。两个独立缺陷：
  ① 五条探针文件通道（settings/launchpad/folder/hardware/notifications）都以「读文件 → 与
  last_command 比较 → 不同则执行」工作，而该记忆启动时为空，于是继承来的隔夜命令被当作新命令执行。
  事故日志实证 `DEBUG [settings] probe command=set behavior.hide_taskbar 1` →
  `INFO [core] taskbar visibility -> 0`。新增 `src/core/probe_file.{h,cpp}`
  （`ReadProbeFileContent` / `ProbeCommandOf` / `ProbeCommandGate::Prime`），启动时把文件现有内容
  标记为已读，五条通道收敛到同一实现；hover 通道故意不 Prime（它承载电平而非事件，Prime 会吞掉
  探针的下一次同值写入）。
  ② 探针用 `Stop-Process -Force` 收尾，即 TerminateProcess，正常退出恢复与
  SetUnhandledExceptionFilter 崩溃恢复都跑不到。新增 `tools/probe_cleanup.ps1`（杀全部探针镜像 +
  恢复 Shell_TrayWnd/Shell_SecondaryTrayWnd + 报告泄漏环境变量），`notifications_probe.ps1` 每条
  退出路径走 `Stop-ProbeCleanly` 并自清 20 个可能继承的 `DOCKFORGE_*` 变量。
- 顺带修掉既有测试缺陷：`tests/shell_window_test.cpp` 的 EnumerateAppWindows 用例在循环内对每个
  实时窗口断言 8 次，断言总数随操作者开几个应用而变（同一 commit 实测 4530 与 4554，差 24 =
  3 窗口 × 8），使 evidence/test.log 无法跨次比较。改为累积违规后断言一次，计数固定。
- 实测：build 全目标 exit 0；全量 456 用例 4544 断言 exit 0 且连跑四次数字完全一致；`[probe]`
  8 用例 21 断言；`[notifications]` 32 用例 181 断言；新增 `tools/stale_command_probe.ps1`
  ALL PASS（植入隔夜命令 → 启动 → 断言未执行且任务栏可见 → 写新命令仍生效，证明通道未被堵死）；
  notifications 探针真实与桩两种模式仍全 PASS，结束后无残留进程。
- 证据：`evidence/T-076/verify.txt` 第八节、`probe_stale_command.log`、`build.log`、`test.log`、
  `test_probe.log`
- 遗留：probe_cleanup 只能恢复被 ShowWindow 隐藏的任务栏，shell 自身状态被改坏仍需重启 explorer；
  脚本不碰注册表与自动隐藏设置。
- 下轮开始于：T-085 社区主题分发替代方案（P1）

## 2026-10-31T23:20:00+08:00 | T-301 | DONE
- 做了什么：Finder 栏左区落地。新增 `src/finderbar/bar_layout.{h,cpp}`（纯函数布局层：`LayoutBar` 两遍算法、`HitTestBar`、`AdvanceAutoHide` smoothstep、`ApproximateTextWidth`）与 `src/finderbar/finder_bar.{h,cpp}`（自有窗口类 `DockForgeFinderBar` + DComp 表面 + `PlateRenderer` + `SetWindowRgn` + 菜单工作线程 + 前台跟踪）。`src/core/config.{h,cpp}` 新增 `FinderBarConfig` 七键（`enabled/height/show_system_menu/show_app_name/show_app_menu/max_menu_items/auto_hide`），走 C-01 钳制 + 未知键保留 + 默认值不写键。`src/main.cpp` 接入装配、tick、config 热重载、系统菜单命令分派、探针通道。`CMakeLists.txt` 加 `version` 库（`GetFileVersionInfoW`）。
- 实测依据（`spikes/T-301_finderbar_menu/`，八个 spike + 一个 fixture）：跨进程 `GetMenu` 无需 hook（Q2）；外来 HMENU 交本进程 `TrackPopupMenuEx` 返回真实命令 id 并往返成功（Q9）；工作线程弹菜单时主线程帧间隔 32.27→31.85ms（Q12）；`GetMenuItemRect` 必须在该线程调用，主线程调用在 192DPI 下返回双倍坐标（Q12 第一次假失败）；`SPI_SETWORKAREA` 返回成功但工作区不变（Q8）；点击穿透必须 layered 且与 DComp 互斥（Q7）；前台归属决定菜单能否被 Escape/外部点击关闭（Q14/Q15/Q18）；本机 11 个可见窗口只有 2 个带 Win32 菜单（Q3，决定"无菜单"是主路径）；`EVENT_SYSTEM_FOREGROUND` 均值 5.5ms / 最坏 8.2ms，名字解析 1.404ms（Q4/Q6）。
- 修掉的真实缺陷（探针抓出，非探针自身问题）：① `Tick` 里 `foreground_dirty_` 在 `RefreshForeground()` 内被清零，之后才读 `foreground_changed` → 恒 false → 前台切换后布局更新但从不重绘（状态 JSON 列出四个菜单标题，屏幕仍是旧应用名、零菜单）；② `SetWindowRgn` 用零面积 region 表示"收起"，而 region 同时是绘制裁剪，把 hairline 一起裁掉；③ `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` 无条件设置，让整条栏在任何截图里完全不可见（采样全是壁纸色）；④ `MenuJob` 整结构赋值在含 `std::atomic` 成员时不可编译；⑤ 匿名 namespace 的同名常量与头文件 `kSystemMenuBase` 冲突导致 `reference to 'kSystemMenuBase' is ambiguous`。
- 实测：`tools/build.ps1 -Action all -Jobs 2` → 全目标 OK，`All tests passed (4681 assertions in 480 test cases)`（基线 475/4654，新增 `tests/finderbar_test.cpp` 5 用例 27 断言）。`tools/finderbar_probe.ps1` → `verdict: Q19_bar_geometry=PASS Q20_auto_hide=PASS Q21_foreign_menu=PASS Q22_forwarding=PASS Q23_menu_lifecycle=PASS`（35 项断言全 PASS）。关键证据：`bar rect = 0,0,2880,56`（28 dip @192dpi）、收起 `progress=0.0 offset=-24.0 interactive=False` 且 `WindowFromPoint` 不命中、展开 `progress=1.0 offset=0.0 interactive=True` 且命中、`titles=[File,Edit,View,Help]` 四项全部有墨（9.84%/11.39%/10.65%/11.61%）、应用名 15.51% 有墨、fixture 收到 `command 4002`、`WM_CANCELMODE` 关掉弹窗后 `menu_open=False paints=19`。
- 诚实边界：① 栏材质固定 Solid 档（ADR-0045），与 dock 的材质档位不再一致，I5 只做到"同一 PlateRenderer + 同样圆角/描边"。② 应用名点击暂不动作（macOS 那里是窗口列表，属 T-204）。③ 中区/右区未实现，`status_width_dip` 恒 0，状态区从栏尾开始。④ 探针用 `SetForegroundOverride` 钉住前台（`attach <hwnd>`），因为探针进程无法替目标窗口抢前台；该接口只替换输入源，命名/读菜单/布局/绘制/菜单线程/转发全走生产路径。⑤ 未在真实第三方应用（Sublime Text 等）上跑过端到端点击，只用 fixture 验证了 C-32 全链路。
- 证据：`evidence/T-301/`（`probe.txt`、四张截图、`dockforge-*.log`、`target-state.txt`、`px_*.txt` 采样）
- 下轮开始于：T-302 Finder 栏中间常驻小组件区（P5）

## 2026-11-01T00:20:00+08:00 | T-301b | DONE
- 做了什么：用户点名的三项缺陷全部落地。①**图标 alpha**：`src/shell/icon_cache.cpp` 新增 `PremultiplyIfStraight`，`IconBitmapFromHBitmap` 在返回前把 shell 给的直通 alpha 转成预乘，`IconBitmap::pixels` 的不变量改为"恒为预乘 BGRA"；`SaveToDisk` 的 `Unpremultiply` 变成真正的逆运算；磁盘缓存键 `v2_`→`v3_`。②**缩放滤波**：`src/render/backend_d2d.cpp` 的 `DrawBitmap` 从 `D2D1_INTERPOLATION_MODE_LINEAR` 改为 `MULTI_SAMPLE_LINEAR`，注释内嵌四模式实测表。③**自绘菜单栏**：`src/finderbar/finder_bar.{h,cpp}` 新增 `DecodeOwnerDrawnTitle`，`ReadAppMenu` 在 `MFT_OWNERDRAW` 且文本为空时用 `ReadProcessMemory(dwItemData)` 读标题；`ForwardMenuItem` 不再把命令 id 0 当"无命令"丢弃（Sublime 的 `File > New File` 就是 0）。新增 `tools/finderbar_owner_probe.ps1`（13 项断言，跑真实 Sublime Text）。
- 实测依据（`spikes/T-301_finderbar_menu/`，六个新 spike）：`alpha_check` 证明 notepad 256px 有 9427 个半透明 texel、6215 个满足 `rgb > a`（`px[6229] B=127 G=255 R=255 A=2`），且自建 straight DIB 经同一 `GetDIBits` 原样返回 → 是 shell 给的就是直通；`interp_check2` 四模式对比，棋盘格 stdev `LINEAR 37.67 / MULTI_SAMPLE 19.19 / CUBIC 0.48 / ANISOTROPIC 0.01`，真实图标 rms_box `5.791 / 1.339 / 1.813 / 5.587`；`icon_fill` 23 个真实图标 fill 86.7%~100%、coverage 10.2%~100% → 否决 bbox 归一化；`menu_spike9/10` 证明 MSAA `OBJID_MENU` 与 UIA `MenuBar` 的子项名称全为空；`menu_spike13` 的 hexdump 证明 `dwItemData` 直接指向 NUL 分隔 UTF-16 标题数组、第 i 项指针落在第 i 个标题首字节。
- 修掉的真实缺陷：① 直通 alpha 被当预乘上传（深色背景上亮边与脏色，这就是"图标不如原生"的根因）；② 无条件 `Unpremultiply` 写盘 + WIC 读回再处理，构成第二重破坏；③ `LINEAR` 在 2.67 倍下采样时丢 texel；④ 自绘菜单栏读不到任何标题（`GetMenuStringW` 返回空）；⑤ `ForwardMenuItem` 拒绝命令 id 0，让真实菜单项点了没反应。
- 实测：`tools/build.ps1 -Action all -Jobs 2` → `All tests passed (4846 assertions in 490 test cases)`（基线 480/4681，新增 10 用例 / 165 断言：`tests/shell_icon_test.cpp` 5 个 + `tests/finderbar_test.cpp` 5 个）。`tools/finderbar_owner_probe.ps1` → `verdict: Q24_ownerdraw_titles=PASS Q25_ownerdraw_forwarding=PASS`（13/13 PASS）。`tools/finderbar_probe.ps1` 回归 → `Q19..Q23` 五项仍全 PASS（普通文本菜单路径未被破坏）。真实应用快照：`foreground_name=Sublime Text`、`menu_titles=File|Edit|Selection|Find|View|Goto`、`placed_menus=6`、`boxes=[20..42][54..129][143..178][196..235][253..324][342..383][401..445][463..509]`、`pick 2 0` → `forwarded WM_COMMAND 0 posted=1`、`pick 2 3` → `sent=0`（分隔符被拒）。
- 诚实边界：① 探针不读应用内部状态（按用户要求"别动我 sublime"），只断言"读到了标题 + 消息发出去了 + 分隔符被拒"，因此"Sublime 真的执行了那条命令"这一步没有仪器证据，只有 `PostMessage` 返回值与日志。② 图标视觉归一化按实测否决（ADR-0048），不是没做而是不该做；如果用户仍觉得小，需要改的是 dock 槽位占比（`dock_view.cpp` 的 slot 计算），属 T-202 蒙版任务的范畴。③ 本机未安装 VS Code，自绘菜单栏只在 Sublime Text 上验证过。
- 证据：`evidence/T-301/`（`alpha.txt`、`interp.txt`、`icon_fill.txt`、`menu_spike13.txt`、`owner_probe.txt`、`dock_after_alpha.png`、`slots_after_alpha.txt`）
- 下轮开始于：T-302 Finder 栏中间常驻小组件区（P5）

## 2026-11-01T04:20:00+08:00 | T-302b + T-302c | DONE
- 做了什么：用户点名三件事全部落地。① **顶栏常驻**：新增 `src/finderbar/appbar.{h,cpp}`，用 shell AppBar 协议（`ABM_NEW`→`ABM_QUERYPOS`→`ABM_SETPOS`）预留顶条带，`FinderBarConfig::auto_hide` 默认从 `true` 翻成 `false`，降为预留失败时的回退；② **Dock 可隐藏**：新增 `BehaviorConfig::dock_visible`（C-01 三处 + settings 字段 `behavior.dock_visible`），`apply_placement` 的 `SWP_SHOWWINDOW` 改为条件式，热键与托盘菜单都持久化；③ **系统托盘图标**：新增 `src/tray/tray_icon.{h,cpp}`（自有窗口类、运行时绘制图标、六项菜单、Quit 走 `WM_CLOSE`），不再需要任务管理器。
- 实测依据（全部本机跑过）：
  - `spikes/T-302_tray/appbar_spike.cpp`：`[Q10] ABM_SETPOS -> 1`、`work area 0,0,2880,1824 → 0,56,2880,1824`、`maximized rect top -13 → 43`、`ABM_REMOVE → work area 回到 0,0,2880,1824`，`verdict: Q10..Q13 四项 PASS`。
  - `spikes/T-302_tray/tray_tree.cpp`：Win11 通知区已是 XAML island（`TrayNotifyWnd` 下无 `SysPager`、无 `ToolbarWindow32`，所有 `buttons=0`）→ 存在性只能用 `Shell_NotifyIconGetRect`。
  - `spikes/T-302_tray/tray_spike3.cpp`：`SetForegroundWindow=0` 时外部点击无效且卡 17 秒；`match=1` 时 Escape 140ms、紧贴右边缘外一点 172ms 均正常。
  - `spikes/T-302_tray/tray_spike4.cpp`：`EndMenu()` 从模态循环的 `WM_TIMER` 调用能终止 `TrackPopupMenu`（1406ms）；`GetAsyncKeyState(VK_LBUTTON)` 无前台权限也能读物理按键。
  - `evidence/T-302/appbar_probe.txt`：`Q34_strip_reserved=PASS Q35_maximize_respects=PASS Q36_release_restores=PASS Q37_autohide_reserves_none=PASS`（`reserved=56px`、`work area 0,56,2880,1824`、`maximized rect=-13,43`、`hide` 后 `reserved=0` 且工作区回到 `0,0`、`auto_hide=true` 时 `reserved=0`）。
  - `evidence/T-302/tray_probe.txt`：`Q30_tray_icon=PASS Q31_tray_menu=PASS Q32_dock_toggle=PASS Q33_tray_quit=PASS`（`icon rect = 2122,1824,2186,1920 (64x96)`、菜单含 `Hide Dock`/`Quit DockForge`、Quit 在最后且不是默认项、`dock_visible` 往返都写盘、Quit 后进程退出且图标已移除）。
  - 回归：`evidence/T-302/finderbar_regression.txt`（`Q19..Q23` 五项 PASS）、`evidence/T-302/owner_regression.txt`（`Q24/Q25` PASS）、`All tests passed (4922 assertions in 505 test cases)`。
- 修掉的真实缺陷（全部是探针抓出来的）：
  1. **变更检测永远不命中**：`ConfigChanged` 订阅者写的是 `if (config.x == event.config.x) return;`，而所有发布者都先把新值写进 `config` 再 publish。后果：「config 说 hidden 而 dock 仍在屏幕上」、「热重载后 `auto_hide` 被接受但永远到不了栏」。修法：比较它自己上次应用的值（`dock_visible_applied`、`finderbar_applied`）（ADR-0052）。
  2. **`ReloadConfig` 不重算预留**：`auto_hide` 从 false 变 true 后栏仍拿着 56px 预留却在收起。修法：末尾按「实际持有量」而非「旧配置值」判断，该释放的释放、该重建的重建。
  3. **`apply_placement` 强制显示**：`SWP_SHOWWINDOW` 硬编码，任何布局重算都会把用户隐藏的 dock 拉回来。修法：条件式。
  4. **热键 `toggle_dock` 不持久化**：只 `ShowWindow` 不写 config，重启就回来。修法：与托盘菜单行为对齐，都先 `SaveConfig` 再 `Publish`。
  5. **探针自身三个错**：`pick 1` 会在主线程弹模态菜单并卡死（改用新增的 `invoke <name>` 直接派发）；`quit` 直接 `PostMessage(WM_CLOSE)` 绕过了真实 handler（改为 `InvokeCommand(Quit)`）；`-not X -contains Y` 在 PowerShell 里解析成 `(-not X) -contains Y`（加括号）。
- 诚实边界：① 预留在本机（单显示器 2880x1920@192dpi）实测成功，**多显示器未测**；`ABM_QUERYPOS` 在副屏上的行为需要后续验证。② 托盘菜单的「外部点击关闭」走的是 `EndMenu()` 自救，不是系统原生行为，因为前台赋权不可靠；真实点击路径未在真人手上测过。③ `dock_visible=false` 时 dock 窗口仍存在（只是 `SW_HIDE`），因为显示时需要原对象恢复；它不占 GPU/CPU，但会占一份窗口句柄。④ 托盘图标的字形是「圆角板 + 三个圆点」，不是 macOS 风格的任何具体形状（避 Apple 商标，ASSETS.md）。
- 证据：`evidence/T-302/{appbar_probe.txt,tray_probe.txt,finderbar_regression.txt,owner_regression.txt,bar_pinned.png,menu_open.png,px_pinned.txt}`
- 关键决策：ADR-0050（AppBar 预留推翻 Q8 的自动隐藏结论）、ADR-0051（托盘图标运行时绘制 + `Shell_NotifyIconGetRect` 证存在）、ADR-0052（变更检测比较已应用状态）。契约：C-31 的 `auto_hide` 默认值翻转、新增 C-35（托盘）与 C-36（Dock 可见性）。
- 下轮开始于：T-302 Finder 栏中间常驻小组件区（P5）

## 2026-11-18T01:20:00+08:00 | T-307 | DONE
- 做了什么：Finder 栏右区框架一次成型 + 时钟先行。① `bar_layout.{h,cpp}` 新增 `LayoutRightGroup`：按 C-31 `finderbar.right=[{type,order}]` 从右往左布局（order 0=最右，同 order 按 C-31 声明序稳定排序、序值小者靠右），输出从左到右排序的盒子与实测总宽；只注册 clock/weather_brief 两种渲染器（`RightTypeRegistered`），其余六种（tray_mirror/control_center/notification_center/ime/battery/network）未注册类型直接跳过不占位，T-303..T-306 增量注册只改一处。② `finder_bar.cpp` `Relayout()`：右区实测宽传入 `LayoutBar` 的 `status_width_dip`（不再恒 0），state JSON 实测 `right_width_dip=158.6`（右区两枚）且左区盒与右区无重叠。③ 时间格式：空 `finderbar.clock.format` 跟随系统区域（`LOCALE_SSHORTTIME` + `LOCALE_SSHORTDATE`），自定义格式串走 `GetTimeFormatEx` 原生图纸；注意规格原文写 `LOCALE_SDATE`，但该常量是日期分隔符而非图纸（实测喂给 `GetDateFormatEx` 只渲染出一个 `/`），完整短日期图纸是 `LOCALE_SSHORTDATE`，代码注释已记录口径修正。④ config：`finderbar.clock.format/show_date/show_seconds` + `finderbar.right` 默认表 `[{type:"clock",order:0}]`（空配置/显式 `[]` 回退默认表；显式表整体替换不追加）进 config.h/.cpp 往返；`FinderBarConfig::right` 成员直接以默认表初始化（空 vector 会破坏「默认序列化不写 finderbar 键」的 C-01 quiet-file 不变量，`config_test` 两例抓住）。⑤ 月历面板 `calendar_panel.{h,cpp}`：仿 notification_panel 的 RegisterWindowClass/Create/Open/Close + PanelHit 命中模式，窗口用 `WS_EX_NOREDIRECTIONBITMAP`+DComp（Solid 自绘板，ADR-0008/0045），度量取 Beautiful NotificationCenterWindow.xaml：360×640 DIP、根圆角 12、内容卡圆角 10、今日蓝圈（systemBlue 浅 #007AFF/深 #0A84FF），颜色 token 对照 Beautiful MacTokens.cs 深/浅两套；月导航 ‹ › + ←/→ 键、周首列跟随 `LOCALE_IFIRSTDAYOFWEEK`。⑥ 天气简显：绘制复用 `dock::DrawWeatherGlyph`（dock_view.h:153/dock_view.cpp:223，grep 全仓仅此三处+本包调用），数据沿用 T-075 `weather::Host` 快照链（main.cpp 每帧 `SetWeatherSnapshot` 推副本，host 启动条件放宽到「dock 天气 tile 或 finderbar weather_brief」），无数据/失败显示 `--°`+Unknown 占位环；点击经探针 `weather` 命令展开 3 天预报（复用 `weather_popup`）。⑦ 探针新增 `calendar`（toggle）/`weather` 命令，走与真实点击完全相同的代码路径。
- 实测依据（全部本机跑过）：
  - `tools/build.ps1 -Action all` → `All tests passed (5195 assertions in 522 test cases)`（T-301 基线 490 cases，新增 32 cases/约 350 断言：`tests/finderbar_layout_test.cpp` 右区 8 + 时钟 4 + config 3 + 月历 2，共 17 个新 TEST_CASE；`config_test`/`config_reload_test` 既有用例回归通过）。
  - state JSON（探针实测）：`clock_text="2026/11/18 0:55"`（跟随系统区域）；`right_boxes=[weather_brief 1269.4-1313.4 icon text='4°', clock 1325.4-1428.0]`——weather 在时钟左侧（order 1 vs 0）、时钟右缘 1428=1440-12（kTrailingInsetDip）✓；占位 run `state_dark_weatherfail.txt` 显示 `text='--°'`（`DOCKFORGE_WEATHER_API_HOST` 指向 dead name 的 T-075 失败路径）。
  - 截图：`bar_calendar_dark.png`（深色：右区天气+时钟并存、日历面板"今天"卡大时钟+日期、2026年11月网格、今日 18 蓝圈、周首列=周一跟随区域）、`bar_calendar_light.png`（浅色面板白底深字同度量）、`bar_corner_dark.png`/`bar_corner_light.png`（右区特写）、`bar_corner_dark_weatherfail.png`（占位）、`weather_popup_dark.png`（点击天气展开"北京 今天/明天/周五"3 天预报，复用 weather_popup）。
  - 点击外部关闭：真实鼠标点击时钟盒（物理 2757,28，探针 state 换算）→ 面板打开 → 真实点击桌面 → state JSON `calendar_visible: false`。全链路（bar WM_LBUTTONDOWN → OnRightClick → Open → SetForegroundWindow → WM_ACTIVATE(WA_INACTIVE) → Close）真实路径验证。
  - 取证脚本：`tools/t307_evidence.ps1`（scratch config + 探针 + DPI-aware 截屏，可复现全部证据）。
- 修掉的真实缺陷（全部是实测抓出来的）：
  1. **`AppendSecondsPicture` 插入点错**：`:ss` 插在 AM/PM 设计符前的空格**之后**（"h:mm tt" → "h:mm :sstt"，实测输出 "5:27 :56下午"）。修：插入点回退跳过空白，落在空格之前。
  2. **`FinderBarConfig::right` 空成员破坏 quiet-file 不变量**：默认空 vector 使 `SerializeConfig(Config{})` 开始写 finderbar 键，`config_test` 两例（unknown-keys 往返稳定性、auto_hide 不写盘）失败。修：成员以 `DefaultFinderBarRightItems()` 初始化 + 显式表解析时先 clear 再填。
  3. **日历面板失活不关**：`Open()` 里 `was_activated_=false` 写在 `SetForegroundWindow` 之后，把同步到达的 `WM_ACTIVATE(WA_ACTIVE)` 置位又擦掉。修序后仍观测到「WA_ACTIVE done=1 后 2.5s 的 WA_INACTIVE 读 was=0」（同 hwnd 同实例、无中间写入，单线程消息循环）——**未定谜团**，已在代码注释与本条记录；终改「失活即关」（panel 自己 claim 前台，未激活过就不可能有 WA_INACTIVE，语义等价且少一个状态位）。
  4. **探针 gate 内容去重**：连续两条相同 `calendar` 命令第二条被拒（gate 只认内容变化），取证脚本用 `state` 命令隔开。
- 诚实边界：① **Esc 键关闭未验证**——面板 `WM_KEYDOWN(VK_ESCAPE)` 已实现，但 SendKeys 依赖真实前台归属，取证环境无法确定性投递；点击外部关闭已验证，Esc 只能人工确认。② 探针路径（主进程不拥有前台）下 `SetForegroundWindow` 被拒时面板保持打开（无 WA_INACTIVE 可关）——与 T-301 菜单 Q6/Q7 的前台竞态同类，真实用户点击路径不受影响。③ **浅色主题下顶栏文字对比度差**（bar_calendar_light.png 顶栏时钟发灰）：T-301 既有行为——`PlateRenderer` Solid 档 tint 固定深色不随主题（render/material.cpp:268 硬编码），右区文字用与左区一致的 text_color，本包未改；要修的是 plate 的主题感知，属 T-005/T-301 域。④ 多显示器未测（panel 钳制在主显示器内，T-203 域）。⑤ amap provider 未实现（T-075 边界维持），本包未新增 provider。⑥ 月历大时钟 34pt 用 `DrawLabel` 固定 SemiBold 权重（backend 无 weight 参数），不是 Beautiful 的 Light——视觉口径差异，待 T-207 主题接管时统一。⑦ 日历面板 640 DIP 高中下方约 160 DIP 留白（月历卡内容自适应，规格只冻结面板外形）。
- 证据：`evidence/T-307/{bar_calendar_dark.png,bar_corner_dark.png,bar_calendar_light.png,bar_corner_light.png,bar_calendar_dark_weatherfail.png,bar_corner_dark_weatherfail.png,weather_popup_dark.png,state_dark.txt,state_light.txt,state_dark_weatherfail.txt,scratch/}`；取证脚本 `tools/t307_evidence.ps1`。
- 关键决策：ADR-0053（finderbar.clock.* 新键 + finderbar.right 默认表 + order 从右往左语义 + 插入约定 + T-207 对接裁决 + 日历面板材质）。ASSETS.md：本包未复制任何 Beautiful 素材文件（仅读取其 XAML/cs 的度量与颜色 token 参数，代码全自绘），无登记义务。
- 下轮开始于：T-303 托盘镜像（按清单顺序串行）

## 2026-11-18T02:00:00+08:00 | T-302（第一会话切片） | DONE
- 做了什么：Finder 栏中央常驻小组件区（监控类五件）落地。① 新增 `src/finderbar/bar_widgets.{h,cpp}`：C-31 八类型注册表（cpu/memory/disk/network/temperature 五类渲染器注册，weather/media/calendar_brief 留给 T-302d，未注册类型跳过不占位）+ 栏内单行紧凑绘制（28 DIP 内、13 DIP 同 kMenuFontDip）+ 色阶三档映射（GradeForPercent 独立可单测）+ 采样引擎 WidgetHost（轻量四类在 bar 既有 Tick 链上 2s 周期采样；温度 WMI 放独立 std::thread，原子 int32 毫摄氏度无锁快照，失败退避 15s、不可用即不渲染）。② 数据通道裁定（ADR-0054）：不扩 C-23 冻结载荷、不改其采样节律——C-23 SystemStatsUpdated 仅面板打开时广播（hardware_panel.h:47），栏内组件需要常驻数据流；采样全不在渲染路径，CPU 差分复用 system_stats 采样函数（ReadSystemCounters/ComputeCpuPercent）。③ 色阶：<80% 主题前景白 / 80–90% 橙 #FF9F0A / >90% 红 #FF453A（macOS HIG systemOrange/systemRed；Beautiful MacOS.xaml grep 9F0A/453A/Orange/Red 全 0 命中，不从此色板取值，复核记录在 evidence）。温度复用同三档读作 °C，network 是速率恒主题色。④ config：`finderbar.widgets`（C-31 冻结键形 [{type,order}]，默认表五件；显式 `[]` 保持空——与 right 的「空回退默认」刻意不同并注明）+ 新键 `finderbar.widgets_refresh_ms`（默认 2000，钳 [2000,60000]，下限即 C-23 0.5Hz 上限）进 config.h/.cpp 解析/钳制/往返/quiet-file。⑤ finder_bar.{h,cpp} 接入：中央区在左区与右区之间居中布局（越界丢盒不挤压、不与两区重叠）、渲染按 grade 着色、Tick 链采样、ReloadConfig 按表内容 re-arm 采样宿、state JSON 新增 widgets 段（probe 可断言 boxes/grade/snapshot）。⑥ 取证钩子 DOCKFORGE_WIDGETS_DEMO=<normal|warn|critical>（同 DOCKFORGE_FORCE_MATERIAL 先例，仅替换输入源）。
- 实测依据（全部本机跑过）：
  - `tools/build.ps1 -Action all` → `All tests passed (5341 assertions in 539 test cases)`（T-307 基线 522 cases，新增 17 cases/146 断言：色阶边界与冻结色值 2、注册表 1、文本格式 1、纯函数 1、BuildWidgetItems 2、中央区布局 4、config 6）。
  - 取证脚本 `tools/t302_evidence.ps1`：`verdict: default PASS reduced PASS off PASS grade_normal PASS grade_warn PASS grade_critical PASS`。default 真实采样 `snapshot={cpu 7.29%, mem 54.43%, disk 93.01%(critical 红), net 1977.5B/s, temperature_valid=false}`，4 盒全部落在 [left_end=190.4, right_start=1329.4] 区间内。
  - 热重载往返（DoD 6）：`reduced` 模式改盘上 config 默认表→[cpu]→默认表，watcher+ConfigChanged+ReloadConfig 全链路实测（state JSON `got cpu` → `got 4`），无需重启。
  - 色阶三档截图（DoD 3）：`widgets_grade_{normal,warn,critical}.png` + `zoom_*` 特写——normal 全白、warn CPU/MEM/DISK/TEMP 全橙、critical 全红，NET 恒白（裁定口径）；default 真实数据本机磁盘 93% 天然呈现 critical 档。
  - CPU 预算（DoD 4，`tools/t302_idle_budget.ps1` + idle_stats.ps1 60s）：idle_off（widgets=[]）0.1529%，idle_on（默认表采样+温度线程）0.1472%（增量 −0.0057pp，噪声内持平），idle_dual（widgets 与 hardware_panel C-23 采样双采样点并存）0.1991%（增量 +0.0462pp）——双采样点均 < 0.3% 预算，降级方案未触发。
  - 温度 WMI 实况（DoD 5）：`Get-CimInstance MSAcpi_ThermalZoneTemperature` → 拒绝访问（wmi_thermal_check.txt）；应用一致 `temperature_valid=false`、温度组件不渲染（default 4 盒）、进程不崩溃、日志一行 WARN；禁 WinRing0（代码零引用）。
- 修掉的真实缺陷（全部实测抓出）：
  1. **demo 温度被 thermal 槽覆盖**：Tick 里 thermal 原子槽合成在 ApplyDemoIfAny 之后，DOCKFORGE_WIDGETS_DEMO 强制的 temperature_valid 被抹掉（三档截图缺 TEMP）。修：thermal 合成前移。
  2. **取证脚本读 state 用 ANSI 码页**：中文前台应用名的 UTF-8 字节被 GBK 读坏 → ConvertFrom-Json 直接炸。修：`-Encoding UTF8`。
  3. **probe gate 内容去重吞掉第二条 state**：两次相同 "state" 命令第二条被 gate 拒（gate 只认全文变化，T-307 同坑），state 文件不重写、读到旧 JSON——热重载第一次假 FAIL。修：每条命令带唯一 token 第二行（ProbeCommandOf 只读首行，probe_file.cpp:34）。
  4. **MinGW MIB2 头文件链**：iphlpapi.h 先定义 __IPHLPAPI_H__ 再含 netioapi.h，跳过其 else 分支，_WS2IPDEF_ 未定义 → GetIfTable2/FreeMibTable/ADDRESS_FAMILY/NET_IFINDEX 全部未声明。修：winsock2+ws2ipdef+nldef+ifdef 前置于 iphlpapi（注释记录）。链接库新增 iphlpapi + wbemuuid。
  5. **取证脚本 Write-Config "default" 生成非法 JSON**：首次 default 模式靠解析失败回退默认侥幸工作、reduced 还原步骤被正确拒绝。修：default 模式整个省略 widgets 键（成员默认表）。
- 诚实边界：① 本机 WMI 温度拒绝访问，温度组件的「真实数据显示」未在本机验证，仅验证「不可用即不渲染不崩溃」+ demo 读数下三档着色正确；有 ACPI 热区权限的机器上真实读数路径未测。② weather/media/calendar_brief 三类渲染器、增删排序 UI 与「截图三张」中的全量态收口在 T-302d（本包切片，DoD 语义不变跨两包验收）。③ network 不参与色阶是裁定口径（对 B/s 分档是编造刻度），非规格原文明示。④ 中央区组件本包不可点（无 hover 无命中测试），点击行为属 T-302d/后续。⑤ idle_dual 的 hardware_panel 采样点成本此前在 evidence/T-068/idle_cpu.txt 测过（稳态 0.23–0.26%），本包复测的是与其并存的增量。⑥ 多显示器未测（栏钳主屏，T-203 域）。
- 证据：`evidence/T-302/{state_default.txt,widgets_default.png,widgets_default_strip.png,state_reduced.txt,widgets_reduced.png,state_off.txt,widgets_off.png,state_grade_normal.txt,widgets_grade_normal.png,state_grade_warn.txt,widgets_grade_warn.png,state_grade_critical.txt,widgets_grade_critical.png,zoom_*.png,idle_off.txt,idle_on.txt,idle_dual.txt,cpu_budget.txt,wmi_thermal_check.txt,data_channel_ruling.md,scratch/}`；取证脚本 `tools/t302_evidence.ps1`、`tools/t302_idle_budget.ps1`、`tools/t302_zoom.ps1`。
- 关键决策：ADR-0054（finderbar.widgets 默认表与显式空语义 + widgets_refresh_ms 新键 + 中央区 order 从左往右 + 数据通道裁定 + 色值出处 + 取证钩子）。ASSETS.md：本包未复制任何 Beautiful 素材文件（仅读取 AppSettings.cs/TemperatureReader.cs 的参数与规则、截图仅看布局），代码全自绘，无登记义务。
- 下轮开始于：T-303 托盘镜像（按清单顺序串行）；T-302d 收口三类渲染器与增删排序。

## 2026-11-18T03:00:00+08:00 | T-302d（T-302 收口切片） | DONE
- 做了什么：T-302 第二会话收口，C-31 八类型全部可渲染。① `bar_widgets.{h,cpp}` 注册 weather/media/calendar_brief 三类渲染器（`WidgetTypeRegistered` 八项全开；会话 1 遗留的「五类注册」断言同步翻转为八）。② **weather**：绘制复用 `dock::DrawWeatherGlyph`（dock_view.h:153 / dock_view.cpp:223，同 T-307 落点；复核意见 wrong-1 同步修订——weather.cpp 无绘制函数），数据复用 T-075 `weather::Host` 快照链（bar `SetWeatherSnapshot` 转发标量进 WidgetHost，glyph kind 沿用右区同款绘制路径），文本契约与 dock tile `WeatherTextFor` 同条（valid→`{temp}°` / 否则 `--°` 占位）；**无数据占位是规格明文，区别于 temperature 的不可用即隐藏**——weather 盒永不为空（`has_icon`+16 DIP 方格，`WidgetBox` 新增 icon 几何，`WidgetItemWidth`/`LayoutWidgetGroup` 与右区同一套 glyph 记账）。main.cpp 宿启动条件放宽到「dock tile ∨ 右区 weather_brief ∨ 中央区 weather」。③ **media 只读简显**：`WidgetHost` 新增媒体工作线程，私有 `MediaSidecarBridge`（hw_sidecar.exe，T-068 GSMTC 传输）2s 周期 `PollSync`，互斥锁发布 {has_session, playing, title}；线程只可能发 `poll`——**DoD「只读不控制、写路径失败不报错到栏面」由结构保证**（发 `media <action>` 的代码在该线程不存在，控制归 T-304b）；无应答/无会话→组件消失，有会话→▶/⏸ 前缀（hardware_panel.cpp:717 同款字形）+ `TruncateMediaTitle` 截断（24 码元+省略号，代理对不劈开）；缺 exe 一次性 WARN+15s 退避，栏面零报错。④ **calendar_brief**：`月/日` 数字（`11/18`，locale 无关；右区时钟已渲染本地化全日期，中央简显只管扫读）。⑤ 增删排序（DoD）：C-31 `finderbar.widgets` 的 type/order 直接生效已由会话 1 的热载链保证，本包实测证明：order 交换与删除经 watcher→ConfigChanged→ReloadConfig 实时生效，**并修掉一个真实缺陷**——原 `Start()` 在宿已运行时早退，热重载把 media 加入已有表时媒体线程永不启动（移除 temperature 则 WMI 线程残留）；现 `Start()` 比较 worker 集合，按需 join/重启，刷新周期变更仍走原子。⑥ 取证钩子 `DOCKFORGE_WIDGETS_DEMO` 新增 `media` 值（合成会话，全量态截图用）。
- 实测依据（全部本机跑过）：
  - `tools/build.ps1 -Action all -Jobs 2` → `All tests passed (5430 assertions in 545 test cases)`（会话 1 基线 5341/539，新增 6 用例/89 断言：weather 占位契约、calendar 文本、media 会话三态、截断含代理对、BuildWidgetItems 占位盒 vs 缺席、glyph 布局几何；会话 1 两个既有用例按八类型注册口径修订）。首次构建即暴露两处测试断言错（会话 1 用例仍断言三类型未注册；`wstring::compare` 的 count 语义），修正后全绿。
  - 取证脚本 `tools/t302d_evidence.ps1`（四模式全 PASS，state JSON 断言非目视）：`verdict: default PASS`（默认表五件监控，三类新组件确认不在默认表）、`minimal PASS`（`[calendar_brief, weather]` 两盒，`calendar text '11/18'`、`weather icon=true text='4°'`、`weather_valid=true`——open-meteo 真实数据，日志 `refreshed city=北京 temp=4C kind=partly_cloudy`）、`full PASS`（八类型全 placed，media 文本首码位 0x25b6=▶、demo 会话入快照）、`reorder PASS`（before `[weather,calendar_brief]` → 换序 `[calendar_brief,weather]` → 删除 `[calendar_brief]`，全程无重启）。
  - 截图三张（DoD 指名落盘 evidence/T-302/）：`widgets_t302d_default.png`（默认五件，本机磁盘 93% 天然 critical 红档、温度不可用不渲染）、`widgets_t302d_minimal.png`（精简两件）、`widgets_t302d_full.png`（全量八件：CPU/MEM/DISK/NET/TEMP/天气图标+4°/▶ Demo Track/11/18）+ 三张 `*_strip.png` 栏面条带。state 文件与 reorder 前后 JSON 在 `evidence/T-302d/`（含 `state_{default,minimal,full,reorder}.txt`、`reorder_{before,after_swap,after_remove}.json`）。**未引用 evidence/T-302/dock_fresh.png（审计实测内容异常：网页卡片非 Dock 画面）。**
  - 媒体读路径真实验证：full 模式日志 `widget host started: 8 entries ... (thermal=1 media=1)` + `hw_sidecar started pid=16976`——sidecar 进程真实 spawn 且 2s 轮询运行；本机无 GSMTC 会话故真实 poll 返回无会话→组件诚实缺席（demo 只替换输入源用于截图）。
- 修掉的真实缺陷：① `WidgetHost::Start()` 的 `running_` 早退使热重载增删**阻塞性组件**（media/temperature）失效——新增 media 永不轮询、移除 temperature 线程残留；改为 worker 集合比较+按需 join/重启（顺带规避 `std::thread` move-assign 到 joinable 的 `std::terminate` 陷阱）。② 取证脚本 `北京` 字面量被 PowerShell 5.1 按 ANSI(GBK) 读成 `鍖椾含` 写进 config → geocode 必败；改用 T-307 先例的 ASCII `Beijing`（脚本内注释记录该坑）。
- 诚实边界：① 媒体「真实数据显示」未在本机验证——本机无 GSMTC 播放会话，真实 poll 路径已实测（进程 spawn、轮询运行、无会话→不渲染），但「曲名上栏」只有 demo 会话的截图证据，需一台有播放器的机器复核（T-068 的 PARTIAL 边界延续）。② 「写路径失败不报错到栏面」由结构保证（无写路径），未做杀 sidecar 进程的专项实测；缺 exe 分支的 WARN+退避路径与「无会话→缺席」同款代码。③ weather 宿启动仍是启动期判定：热重载新加入的 weather 组件显示 `--°` 占位到下次重启（T-307 既有边界，未扩大）。④ bar 与硬件面板同时配置 media 时存在两个 sidecar 进程（各自生命周期，无共享状态）——ADR-0055 记录的诚实代价。⑤ 三类新组件中央区不可点（会话 1 边界延续）；media 点击控制归 T-304b。⑥ 多显示器未测（T-203 域）。
- 证据：`evidence/T-302/{widgets_t302d_default,minimal,full,reorder}.png + *_strip.png`（截图，DoD 指名目录）；`evidence/T-302d/{state_default.txt,state_minimal.txt,state_full.txt,state_reorder.txt,reorder_before.json,reorder_after_swap.json,reorder_after_remove.json,scratch/}`；取证脚本 `tools/t302d_evidence.ps1`。
- 关键决策：ADR-0055（八类型收口、默认表不收编三类的三条理由、weather 占位 vs temperature 隐藏的规格区分、media 只读的结构保证、双 sidecar 实例裁定、calendar 数字口径、worker 按需启停修正、demo 钩子 media 值）。ASSETS.md：本包未复制任何 Beautiful 素材文件（weather glyph 与全部绘制自绘，仅沿用既有参数），无登记义务。
- TASKS.md：T-302 状态改 DONE（M2 行同步）。
- 下轮开始于：T-303 托盘镜像（按清单顺序串行）

## 2026-11-18T03:15:00+08:00 | T-306 输入法/电池/网络状态指示 | DONE
- 做了什么：右区三个状态指示器按 T-307 右区框架收口。① **新模块**：`src/finderbar/status_sampler.{h,cpp}`（数据+纯映射+`StatusHost` 1s 采样宿）、`status_icons.{h,cpp}`（电池/Wi-Fi/以太网字形，纯圆角矩形几何随 slot 缩放）、`status_popup.{h,cpp}`（点击展开的详情卡，notification_panel 窗口模式 + calendar_panel 的 DComp/ADR-0008 材质路径）。② **ime**：`ImmGetDefaultIMEWnd(被跟踪窗口)` + `WM_IME_CONTROL(IMC_GETCONVERSIONMODE=0x0001)` 读态（SMTO_ABORTIFHUNG+100ms 截止，跨进程挂死不拖 Tick 链），文本 A/中（NATIVE 位判定）；点击 `IMC_SETCONVERSIONMODE=0x0002` 翻转 NATIVE 位、其余位保留，置后立即重采样同帧翻转标签。③ **battery**：`GetSystemPowerStatus`，BatteryFlag 128/255→整项不渲染；BatteryLifePercent 255→0%（Beautiful 同约定）；<20% 单档变红 #FF453A（复用 T-302 冻结 kWidgetCriticalColor，规格只钉红档无橙档）；电量文字与 25×12 外框图标并排。④ **network**：GetAdaptersAddresses（OperStatusUp+网关；IfType 71→Wi-Fi、6→以太网）+ Network List Manager `IsConnectedToInternet`（NLM 拒答退回网关答案、明说断网则诚实显示）+ wlanapi `WlanQueryInterface(current_connection)` 信号 0-100→0-4 格（WifiBarsForQuality 四等分，WlanGetAvailableNetworkList 扫描明确不进 Tick 链——那是 T-304b 的列表）。离线画暗弧不画插头。⑤ **status_popup**：电池/Wi-Fi/输入法三卡详情（电量+充电态、类型+SSID+信号格数、转换模式+切换提示），点外失活/Esc 关闭，宽 320/圆角 12/卡 10 与 calendar_panel 同节奏；WiFi 网络列表明确留给 T-304b。⑥ **config**：`finderbar.right` 默认表收编 `{clock,0},{network,4},{battery,5},{ime,6}`（ADR-0053 预先冻结的位次）。⑦ 刷新固定 1000ms 常量独立于 T-302 的 2s WidgetHost，采样在 Tick 链不进 Paint（C-23/ADR-0054 同裁决）。
- 实测依据（全部本机跑过）：
  - `tools/build.ps1 -Action all` → `All tests passed (5502 assertions in 556 test cases)`（T-302d 基线 5430/545，新增 11 用例/72 断言：格数映射 0/24/25/49/50/74/75/99/100+越界钳制、电量两档色阶含边界 20 与负值、BatteryFlag 128/255 隐藏、充电判定、A/中标签与翻转保位、连接类型命名、25 DIP 图标盒布局+alert 透传、icon-only/text-only 条目、popup 高度分段、弧形几何；T-307 两个既有用例按新注册/新默认表口径修订）。
  - IME 协议常量实证：spikes/T-306_status/ime_probe.cpp 实测 `GETCONVERSIONMODE(0x0001)` 读到 0x0（A）、`SETCONVERSIONMODE(0x0002)` 翻转后回读 0x1（中）→ `FLIP VERIFIED`、`GETSENTENCEMODE(0x0003)` 是另一个量（0x8）；常量出处 SDK immdev.h（tpn/winsdk-10 镜像，行 994）。输出存 `evidence/T-306/ime_probe.txt`。
  - 取证脚本 `tools/t306_evidence.ps1` 七模式全 PASS（state JSON 断言非目视）：`real`——默认表四类渲染、左→右 ime<battery<network<clock（order 6/5/4/0）、真实采样 `ime(valid label=A mode=0) battery(99% charging) network(Wi-Fi online bars=3)`、**1Hz 实测**：两次 state 读之间 samples 5→12、采样器自身 GetTickCount64 间隔 7047ms/7 次=1174ms/次≈0.851Hz（≤1Hz DoD，verdict_real.txt sampling 行）、`ime` 探针点击 0x0→0x401 再点还原（即时切换+回读一致）、`status` 探针开/关 popup（status_popup_visible true→false）；`low`——15% 红态；`nobattery`——电池盒消失、ime+network+clock 保留；`battery`——87% charging；`wifi4`——100%→4 格；`wifi2`——60%→2 格；`offline`——未连接全暗弧。
  - 截图落盘 evidence/T-306/（DoD 指名目录）：`bar_status_real.png`（真机 99% 充电+Wi-Fi+A+时钟）、`bar_status_low.png`（**15% 红态**）、`bar_status_nobattery.png`（**无电池隐藏态**）、`bar_status_battery.png`（87% 带 bolt）、`bar_status_wifi4.png`/`bar_status_wifi2.png`（弧数差异）、`bar_status_offline.png`（断链暗弧）、`popup_open_real.png`（详情卡全景：99% 充电中 / Wi-Fi chaomeng-5G·信号 3/4 格 / 输入法 A 切换提示）+ `state_*.txt`/`verdict_*.txt` 七组。
  - 度量对照：`evidence/T-306/metrics_round10.md`——round10-controlcenter.png 真图（电池≈25×12 DIP 外框+百分数、Wi-Fi 弧、16×16 状态图）、round5/02 放大图（充电 bolt、「以太网」命名）、TopBarWindow.xaml.cs（UpdateBattery 的 flag 判定/255→0、NewStatusImage 16）、Beautiful.Core Wifi.cs（格数阈值）、MacOS.xaml grep 状态键 0 命中（故红/橙沿用 T-302 HIG 冻结值不从其取）。
- 修掉的真实缺陷：① **右区带图标条目文本整盒居中压住图标**——battery 的「99%」画在电池外框上（bar_status_real 第一版截图可见，weather_brief 同路径同病）；修为居中于图标右侧子矩形。② **取证脚本重复 state 命令被 probe gate 去重吞掉**（T-302d 同坑再现）——第二条 state 被拒、脚本读到旧文件，1Hz 记录因此假阴（samples 5→5）；修为每条命令带唯一 token 第二行（probe_file.h 约定）。③ **采样率测量口径**：初版拿 s1 快照与 1s 后的 wall clock 相比得出假阳性 1.18Hz；改用采样器自身 last_sample_tick_ms 间隔做诚实括弧（1174ms/次 ≤1Hz 实测）。④ demo wifi4 值错：quality 90 按四等分映射是 3 格不是 4 格，改 100。⑤ offline 态初版误画以太网插头（kind=None 落进 else 分支）——插头会谎称有有线连接，改画 Wi-Fi 形状全暗弧。
- 诚实边界：① 「点击展开对应面板」DoD 的交互路径经探针命令（`status`/`ime`，走 OnRightClick/StatusHost 真实代码路径）验证，未做真人鼠标点击截图（探针先行是 T-307 calendar/weather 同款先例，合成鼠标事件驱不动前台声明面板）。② 真实 <20% 电量与无电池在真机不可自然产生（本机 99% 充电中、有电池），两态为 demo 钩子定格，真值路径（GetSystemPowerStatus 读数）在 real 模式真实可见（99% charging 与系统托盘读数一致）。③ `IsConnectedToInternet` 走 Network List Manager COM，NLM 拒答（COM 初始化失败/服务缺席）时回退网关推断——该回退路径本机未触发（NLM 正常应答 online=true），代码按三态设计。④ 日文/韩文 IME 的 NATIVE 位同源（IME_CMODE_JAPANESE/HANGEUL=0x1）但标签恒为「中」——规格钉 A/中两值，あ/가 属 T-310 i18n 域。⑤ WiFi RSSI 的 dBm 形态未用（wlanSignalQuality 是 0-100 百分比，规格「RSSI」按其接口语义落地为信号质量；dBm opcode 存在但与格数映射的线性刻度不匹配）。⑥ 采样间隔 1174ms>1000ms 名义值：累加器重置不进位 + 帧调度量化所致，方向安全（≤1Hz 是规格上限）。⑦ 多显示器未测（栏钳主屏，T-203 域）。
- 证据：`evidence/T-306/`（七组 state/verdict、八张截图、ime_probe.txt、metrics_round10.md、scratch/）；取证脚本 `tools/t306_evidence.ps1`；协议 spike `spikes/T-306_status/ime_probe.cpp`。
- 关键决策：ADR-0056（三类渲染器注册、默认表收编位次、固定 1s 周期不开配置键、采样通道、IME 常量出处与实证、电池两档裁定、网络三源、status_popup 材质与尺寸、demo 钩子、渲染修正）。ASSETS.md：本包未复制任何 Beautiful 素材文件（全部图标自绘、截图仅度量对照），无登记义务。
- TASKS.md：T-306 状态改 DONE（M2 行同步）。
- 下轮开始于：T-303 托盘镜像（按清单顺序串行）。

## 2026-11-18T04:10:00+08:00 | T-304（第一会话） | DONE
- 做了什么：① 栏内控制中心入口——`RightType::ControlCenter` 注册（bar_layout.cpp）、默认表收编 `control_center=1`（config.cpp DefaultFinderBarRightItems，ADR-0057）、自绘双拨杆 glyph 16 DIP（DrawControlCenterGlyph，hardware_panel.cpp，ASSETS.md 合规零素材复制）。② `hardware_panel.{h,cpp}` 原地升级为 Sonoma 控制中心（类名/窗口类/控制 API/探针命令/SnapshotJson 旧键保留，T-068 探针链不回归）：GDI→DComp（status_popup 同款窗口模型），348 DIP 卡/根圆角 18/#16FFFFFF 描边/三段玻璃渐变 #C838383A→#B8242426→#A8161618 叠 #4D202020（MacOS.xaml 配方），连接卡 2×2（Wi-Fi/蓝牙/勿扰/夜览，圆钮 28×28，开 #0A84FF/关 #3DFFFFFF）+ 音量竖滑杆 26×118（thumb 26×4 填充上缘把手）+ 亮度横滑行 + C-23 错误条；度量全表落 evidence/T-304/metrics_cc.md。③ 音量 SetMasterVolume 实时生效；亮度 WMI 优先 DDC/CI 兜底（spike 实测本机 DDC gle=1168 不支持、WMI 62/101 可用——规格的「若支持」口径据此落双路径，ADR-0057）；WiFi/蓝牙 SetRadio 实机正向验证（T-068 写路径首获实机确认：1↔2 双向翻转+复原，RadioState 枚举）；勿扰=注册表 ToastNotificationEnabled+WM_SETTINGCHANGE 广播（1→0 回读）；夜览=CloudStore blob 0x12 字节翻转（严格 CB 头校验），本机键缺失→面板内提示如实取证。④ 右键最小菜单（finderbar-parity I2 承接）：WM_CONTEXTMENU→TrackPopupMenuEx 六项 ms-settings: 聚合入口，实拉起 SystemSettings 验证后关闭；托盘右键 T-303/R2、时钟/通知/状态图标右键 T-312 不含项。⑤ 关闭：点外部（真实激活后失活即关）+ Esc（真激活后 SendKeys 验证）+ cc_hide 全 PASS。⑥ 不接管系统 OSD（C-34 禁止项未触碰，无任何 OSD 代码）。
- 关键缺陷（已修）：① 连接卡 name/state 文本矩形把绝对底边当 w/h 传入（Rect{x,y,w,h} 语义误用），文字纵移出卡——截图驱动修复并改为与圆钮垂直居中对齐；② 取证脚本坐标系：应用 DPI-aware 物理坐标 vs PS 探针 ÷2 虚拟化坐标，点击算式须减半宽+右缘钳制（panel_rect_probe3.ps1 实测窗口矩形定位）；③ T-068 的 PollSync(600ms/2.5s) 会阻塞与栏共享的帧循环，无线电确认改异步（乐观翻转+3s tick 轮询纠正），探针路径保留有界确认；④ SetMasterVolume/ToggleMasterMute 重写时漏定义致链接失败（补齐）。
- 证据：evidence/T-304/{verdict_real.txt,cc_panel_open.png,cc_panel_zoom.png,bar_cc_button.png,state_bar.txt,state_panel_open.txt,metrics_cc.md,scratch/}；spikes/T-304_cc/{ddc_registry_spike.{cpp,exe,txt},wmi_brightness_probe.{ps1,txt},panel_rect_probe3.ps1}；tools/t304_evidence.ps1（19 断言全 PASS，含独立 WMI/注册表/sidecar 复读）
- 关键决策：ADR-0057（默认表收编 control_center=1、in-place 升级 hardware_panel 保探针契约、恒深色玻璃裁定、亮度 WMI/DDC 双路径、勿扰/夜览注册表口径、无线电异步确认）。
- 遗留：T-304b（媒体控制行/WiFi 列表/逐应用音量/滚轮绑定 I3）；勿扰的「系统层实时生效」只验证到注册表值+广播（toast 抑制的视觉确认无人值守环境不可测，如实未验证）；夜览在本机不可切换（键缺失，DoD 失败提示口径，有键机器走 blob 翻转）；浅色系统主题未单独截图（面板恒深玻璃为 ADR-0057 冻结裁定，浅/深双截图归 T-304b）。
- 下轮开始于：T-304b

## 2026-11-18T06:05:39+08:00 | 冲刺收尾 T-307+T-302+T-302d+T-306+T-304 | DONE
- 做了：五包收口回填台账。① T-307（TASKS.md TODO→DONE，ADR-0053）右区框架+时钟先行：clock/weather_brief 渲染器、LayoutRightGroup 右起布局接入 status_width、月历面板 360×640/圆角12/今日蓝圈、finderbar.clock.* 三键与 finderbar.right 默认表进 config；右区实测宽 right_width_dip=158.64/154.62 两主题，天气失败占位 --°（DOCKFORGE_WEATHER_API_HOST=dead 实测）。② T-302+T-302d 中央小组件区两会话收口（ADR-0054/0055；T-302 状态已在 T-302d 会话改 DONE）：五类监控+色阶三档（HIG #FF9F0A/#FF453A）、2s 采样空闲 CPU 增量实测 +0.0462pp<0.3%、温度独立线程只走 WMI（本机热区拒绝访问→不渲染不崩溃）；收口补 weather/media/calendar_brief 三渲染器、增删排序热重载（修 WidgetHost::Start() running_ 早退真实缺陷）、截图三张。③ T-306（ADR-0056）状态三件套：ime 读态+点击翻转（immdev.h 常量本机 spike 实证）、电池 <20% 红/无电池隐藏、网络三源（GetAdaptersAddresses+NLM+wlanapi，真机 Wi-Fi 85%→3 格）；1s Tick 采样实测 0.851-0.901Hz≤1Hz。④ T-304 第一会话（ADR-0057）控制中心：栏按钮+Sonoma 面板（348 DIP 卡/三段玻璃/2×2 连接卡 28×28/竖滑杆 26×118），音量 0.400 独立复读、亮度 WMI 优先 DDC 兜底（本机 DDC gle=1168 不支持、WMI 62/101 可用）、WiFi/蓝牙 SetRadio 实机双向翻转（T-068 写路径首获实机确认）、勿扰注册表+WM_SETTINGCHANGE、夜览本机键缺失走面板内提示、右键菜单实拉起 SystemSettings、OSD 零触碰。
- 实测依据：门禁数字取自脚本实施结果（本收尾未复跑构建）：T-307 5195 断言/522 用例、T-302 5341/539、T-302d 5430/545、T-306 5502/556、T-304 5572/563 全绿；b540e57 提交信息载明全仓 5590 断言/566 用例。取证 evidence/T-307、T-302、T-304、T-306 本轮 ls 核对在盘；evidence/fidelity-sprint/ 三张 *_fixed_r2.png 未跟踪，随脚本入库。
- 视觉验收（脚本给定 r2 结果，6 区过 1）：过：中央小组件区（中央为空与参考一致）。未过：时钟日历区（三张新截图均无日历/通知浮层，半区无法比对）、中央小组件扩展区（小组件右侧多一枚参考没有的约 14px 白色圆环 glyph）、状态图标区（电池白身内 3 黑点斜排 vs 参考左下 1 点；音量单弧 vs 参考双弧约 32px；铃铛贴片偏亮且多 1 白点）、控制中心区（四方格入口已对齐，面板本体连续三轮无样本）、整屏（左区无前台应用塌缩成单图标无菜单、废纸篓亮色纸团 vs 参考灰网状、运行点本轮无样本、顶栏底色 13,15,18 vs 参考 23,23,24 轻微）。本轮确认已修复：右簇八件完全同序、CC 贴右缘、小组件两行堆叠不裁切、Dock 玻璃三段透出与底缘发丝线对齐。
- 诚实边界：T-307 Esc 关闭子句依赖真实前台归属未确定性验证（点击外部关闭已验证）；T-302 温度真实 WMI 读数需有热区权限机器；T-302d media 真实曲名需有播放器机器（本机无 GSMTC 会话，DOCKFORGE_WIDGETS_DEMO 合成截图为证）、杀 sidecar 专项未实跑；T-304 DDC/CI 写路径无外接显示器未验证、勿扰 toast 抑制无人值守不可测、夜览本机键缺失、浅/深双主题截图归 T-304b。
- 推送：T-307 会话推送 rc=128 失败一次；实测 origin/main=65e37f3（五包提交均已推达），b540e57 与台账提交待脚本统一推送。
- 下轮候选：r2 验收遗留（电池黑点、音量双弧、小组件右侧圆环、铃铛贴片、日历/CC 浮层点击取证、左区菜单形态、废纸篓材质）与 T-304b/T-303/T-305。


## 2026-11-20T02:30:00+08:00 | T-085（社区主题分发替代方案） | DONE
- 做了什么：按 ADR-0026 落地社区主题分发的完整链路，零 Steam 依赖。① 索引层 `src/core/theme_index.{h,cpp}`：静态 JSON 索引（schema 1）解析 + 拉取，字段 name/author/version/preview_url/download_url/sha256/size_bytes 全解析全校验（坏字段整份拒绝、重名拒绝、sha 归一小写、主题名白名单 [a-z0-9._-] 且拒内建名）。② 安装层 `src/core/theme_install.{h,cpp}`：WinHTTP 二进制下载（`platform::HttpGetBytes`，http/https、NO_PROXY、有界失败）→ bcrypt SHA256 全量校验（失败拒绝安装，实测 P4）→ vendored miniz 解包到 `<ThemesDir>\<name>.staging-<tick>`（条目名反斜杠归一+逐级建父目录+穿越守卫）→ C-02 白名单校验（.json/.png/.../字体，theme.json 必须可与 T-052 同解析；实测 P5 带 .exe 的包 SHA 正确仍被拒）→ 原子换名安装，卸载拒内建名。③ 设置窗口 Themes 分区（Dock/Launchpad/Appearance/**Themes**/Behavior/Shortcuts/About）：Index URL 编辑框（C-01 新键 `themes.index_url`，256 字节钳制、空回默认、quiet-file）+ 远程索引列表 + Refresh/Install + 本地列表（built-in 标注/[active]）+ Enable/Use default/Delete/Open folder；异步 worker（shared_ptr 状态 + PostMessage，窗口中途销毁安全），全部动作与按钮调同一方法、探针通道（theme-refresh/install/enable/disable/delete/open-folder）驱动同链。④ 仓库自带 `theme-index/`（index.json + packs/mono-1.0.0.dockforge-theme + assets/mono-preview.png），默认 URL 指向 raw.githubusercontent.com/chenmoulaile/DockForge/main/theme-index/index.json —— 推送后即为可达的真实内容（ADR-0026「索引放仓库」的落地；Pages 未开通，raw 托管为等价静态文件，理由在 verify.txt）。
- spike 结论（spikes/t085_theme_dist/）：shell zip 解包路线被证据否决——`IShellDispatch::NameSpace` 对 .zip 在本控制台进程恒 S_FALSE/E_FAIL+null（字节相同的文件 PowerShell 下正常，进程形态相关），BHID_SFObject 报 SFGAO_FOLDER=0，`.dockforge-theme` 扩展名首调 E_FAIL；zipfldr CLSID 已注册但 `.zip\shellex` namespace 键缺失。选型 vendored miniz（third_party/miniz 拆分文件集+本地 miniz_export.h 存根，与 B-003 vendoring 先例一致）。另实测 PS5.1 Compress-Archive 写反斜杠条目名（`icons\sample.png`）→ 安装器归一+建父目录，并有单测锁定；WinHTTP 对 raw.githubusercontent 用 DEFAULT_PROXY 间歇 12002 超时而 NO_PROXY 5/5 过 → 新代码统一 NO_PROXY（T-075 天气链是既成事实不改动）。
- 实测依据：① 单测 `tests/theme_dist_test.cpp` 19 用例 133 断言全绿（zip 写手造 store-only zip 实测解包/穿越拒绝/C-02 校验/安装/SHA 拒装无残留/重装替换/卸载/config roundtrip/HttpGetBytes 无 scheme 拒绝/SettingsModel 字段）；② 探针 `tools/theme_dist_probe.{cpp,ps1}` VERDICT=PASS——Part A 生产模块 P1..P6 全 PASS（回环 HttpListener origin），Part B 真实 app 经 probe 通道 B1..B9 全过（Index loaded 1 theme → Installed 'mono' → config.json `"theme":"mono"` 持久化 → disable 回 default → delete 目录消失 → open folder 日志），截图 themes_section_light/dark.png（Themes 分区全景：URL 框/远程列表 mono 行/Refresh/Install/本地列表 default[built-in][active]+dark+mono/四按钮）；③ 门禁 `tools/gate.ps1 -Action all` exit 0：5730 断言全过、585 用例 = 583 过 + 2 显式 SKIP。
- 顺带修复既有失败（非本任务引入，基线 97e24dd worktree 复跑同挂）：dock_indicator_test 两个用例在本 harness 会话挂——根因实测 `DWMWA_CLOAKED=1`（cloak_probe.exe：新显示窗口 visible=1 cloaked=1，整个会话的窗口被命令外壳 cloak；IsAppWindow 排除 cloaked 是 T-011 的刻意产品行为）。处置：两用例加 `SessionCloaksItsOwnWindows` 环境守卫，命中 SKIP 并打印原因（普通桌面守卫为 false 断言照常跑），产品代码零改动，test.log 可见 skipped 明细。
- 诚实边界：① 预览图（preview_url）只解析保存，浏览器列表不渲染图片（v1.1 打磨）；② 安装/刷新无进度条，状态行文本反馈，同类任务并发被拒；③ 删除无确认对话框（先选中再删除两步操作，包可从索引重装，内建名拒删）；④ 探针 Part B 走 probe 文件通道（与按钮同方法链），未做真人 BM_CLICK（T-062 已验证控件转发机制）；⑤ 深浅截图差异仅在 D2D 导航配色，PrintWindow 在本机抓不到该子面（HEAD 同样，既有环境限制，verify.txt 第四节）；⑥ 默认 URL 公网可达性：f077f39 已推送（65e37f3..f077f39 rc=0），推送后 tools/t085_default_url_check.ps1 实测 R1 真实 app 从 raw.githubusercontent.com 拉到索引、R2 装包、R3 mono\theme.json 落盘（evidence/T-085/default_url_check.txt + default_url_app_log.txt）。
- 证据：evidence/T-085/（verify.txt 逐条 DoD、probe.txt、app_log.txt、build.log gate exit 0、test.log 5730 断言、双截图）；spikes/t085_theme_dist/（findings.md + spike_run.log）；TASKS.md：T-085 TODO→DONE（M4 行同步、P1.5 定义勾选）。
- 下轮候选：T-303 托盘镜像 / T-304b 控制中心收口 / T-305 通知中心整合。
