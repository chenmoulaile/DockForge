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
