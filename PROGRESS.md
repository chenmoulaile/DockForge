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
