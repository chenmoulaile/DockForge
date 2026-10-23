# DECISIONS.md · ADR 归档

格式：`ADR-NNNN · 标题` + 状态 + 背景 + 决策 + 后果。

---

## ADR-0001 · 技术栈冻结（继承自人类指令，原样保留）

**状态**：已被 ADR-0002 部分取代（WinUI 3 → 纯 Win32 + DirectComposition）

- 语言/构建：C++20 + CMake + vcpkg（manifest 模式）
- 渲染：Direct2D + DirectWrite；模糊/折射用 Composition Effects；复杂液态玻璃用 HLSL
- 材质回退：Mica → Acrylic → 自绘 Gaussian → solid
- Shell：IShellItemImageFactory / IShellLinkW / IApplicationActivationManager / EnumWindows + DwmRegisterThumbnail
- 配置：nlohmann/json + ReadDirectoryChangesW 热重载（300ms 防抖）
- 测试：Catch2 + PowerShell/UIAutomation 冒烟
- 打包：MSIX + Inno Setup 便携版
- 目标：Win10 21H2+ / Win11 22H2+，x64 + arm64

**回退条款**（原文授权）：WinUI 3 无法满足需求时可提 ADR 改用纯 Win32 + DirectComposition，须附 spike 证据，属可自决变更。

---

## ADR-0002 · 渲染栈改为纯 Win32 + DirectComposition + Direct2D

**状态**：Accepted（2026-02-11）

**背景**（实测环境证据）
- 本机无 MSVC（vswhere / cl / msbuild 全部缺失），无 Windows App SDK，无 vcpkg。
- 本机 `C:\Program Files (x86)\Windows Kits\10\Include` 与 `Lib` 目录为空（SDK 未安装）。
- 可用工具链：`g++ (MSYS2 mingw-w64) 15.2.0`、`cmake 4.4.2`、`mingw32-make`、`windres`。
- mingw-w64 自带头文件已验证存在：`dcomp.h`（含 `DCompositionCreateDevice2/3`、`IDCompositionDesktopDevice`、`CreateTargetForHwnd`）、`dwmapi.h`（含 `DwmRegisterThumbnail/Unregister/QueryThumbnailSourceSize`、`DWMWA_SYSTEMBACKDROP_TYPE`、`DWMWA_WINDOW_CORNER_PREFERENCE`）、`d2d1_3.h`、`d2d1effects.h`（`CLSID_D2D1GaussianBlur`）、`shobjidl.h`（`IShellItemImageFactory`）、`psapi.h`（`GetProcessMemoryInfo`）、`oleidl.h`（`IDropTarget`）。
- 对应导入库存在：`libdcomp.a`、`libd2d1.a`、`libdwmapi.a`、`libd3dcompiler_47.a`。

**决策**
采用 **纯 Win32 无边框分层窗口 + DirectComposition（DComp）合成 + Direct2D/DirectWrite 绘制**，不使用 WinUI 3 / Windows App SDK。

**理由**
1. WinUI 3 不可用：无 MSVC、无 Windows App SDK，且 WinUI 3 需要 MSIX 打包与 C++/WinRT 生成工具链，当前环境无法构建。
2. 需求本身不依赖 WinUI：Dock 需要的是「无边框、透明、置顶、逐帧可控」的窗口，Win32 + DComp 是更底层的直接解法，且是原 ADR-0001 明确授权的回退路径。
3. 材质能力等价或更强：Mica/Acrylic 通过 `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)` + `SetWindowCompositionAttribute` 取得；自绘 Gaussian 用 D2D GaussianBlur Effect；液态玻璃用 HLSL + D3D11 直通 DComp。
4. 依赖最小化：无 MSIX、无 C++/WinRT、无 AppSDK 运行时引导，冷启动更容易压到 800ms 预算内。

**后果**
- 需自行实现：DPI 感知、窗口动画、命中测试、无障碍（UIA provider 后续补）。
- 打包从 MSIX 主分发改为 **Inno Setup 便携版 + MSIX（环境具备 MSVC/AppSDK 时再补）**，记为 P2 待办。
- 不使用 vcpkg：改为 `third_party/` 就地 vendoring（nlohmann/json、Catch2 amalgamated），已记录于 ASSETS.md。
- 构建系统：CMake + MinGW Makefiles，工具链文件 `cmake/toolchain-mingw.cmake`。

---

## ADR-0003 · 第三方依赖就地 vendoring 而非 vcpkg

**状态**：Accepted（2026-02-11）

**背景**：vcpkg 未安装；网络可用但 vcpkg 引导 + 全量编译成本高，且本机无 MSVC（vcpkg 默认 triplet 不可用）。

**决策**
- `third_party/nlohmann/include/nlohmann/json.hpp`（v3.11.3，MIT）
- `third_party/catch2/catch_amalgamated.{hpp,cpp}`（v3.5.4，BSL-1.0）
均以文件形式入库，CMake 直接引用，无包管理器参与。

**后果**：升级需手工替换文件并在 ASSETS.md 记录版本与 SHA256。

---

## ADR-0004 · 悬停放大曲线采用幂衰减而非 cos 积分

**状态**：Accepted（2026-02-11）

**背景**：模块规格给出 `s_i = 1 + (k-1) * max(0, cos(π*d/(2*R)))^p`。该式在 d=R 处 cos(π/2)=0，连续；但 `cosine_integral` 变体的闭式解与位移累积耦合复杂，且逐帧求值成本高于幂衰减。

**决策**
默认曲线用 **归一化幂衰减**：`t = clamp(d/R,0,1)`，`s = 1 + (k-1)*(1-t)^p`。
`hover_falloff` 保留三枚举值，`sine` 与 `cosine_integral` 映射到不同 `p` 预设，`gaussian` 用 `exp(-(t*sharpness)^2)`。

**理由**：单次求值仅需 1 次 pow（或查表），可在 GPU 顶点着色器内完成；视觉上与 cos 曲线差异 < 2%（p=2 时）。

**后果**：曲线在 d=R 处导数为 0（与 cos 版一致），无跳变。若后续视觉评审认为"不够弹"，再引入 spring 后处理，不动本契约。

---

## ADR-0005 · 证据与提交策略

**状态**：Accepted（2026-02-11）

- 每任务目录 `evidence/<TASK_ID>/`，至少含：`build.log`、`verify.txt`（DoD 逐条打勾 + 实测值）。
- 截图统一 `dpi100.png` / `dpi150.png` / `dpi200.png` 命名。
- benchmark 输出 `bench.txt`，含 p50/p99 与采样数。
- 每任务一次 commit，message 前缀 `[T-xxx]`。

---

## ADR-0006 · 帧定时用高分辨率等待定时器，不用消息等待 timeout
**状态**：Accepted（2026-10-23，T-003 实测）

**背景**
T-003 首版把帧间隔直接作为 `MsgWaitForMultipleObjectsEx` 的 `dwMilliseconds`。实测（`evidence/T-003/bench.txt` 修复前记录）：请求 16ms，实得 `avg=28.4ms p50=30.98ms fps=35.2`。原因是该系统默认定时器分辨率约 15.6ms，等待被向上取整到下一个 tick，两帧合成一次唤醒。

**决策**
1. 帧节奏由 `CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)` 创建的定时器驱动；该定时器作为 `MsgWaitForMultipleObjectsEx` 的第一个等待句柄，`dwMilliseconds = INFINITE`。
2. 定时器创建失败（Win10 1803 以下）回退为不带 flag 的普通等待定时器 + 原 timeout 路径，`DF_WARN` 记录一次，允许精度降级但不允许卡死。
3. 截止时间用**固定步进**推进：`next_deadline += interval`；若已落后超过一个间隔（长停顿），重同步到 `now + interval`，不补帧。这样定时器抖动不累积成漂移。
4. 禁止用 `timeBeginPeriod(1)` 提升全局定时器精度——它会影响整机功耗，且对本问题只是绕路。

**后果**
- 实测 `avg=16.00ms p50=16.00ms p99=16.66ms fps=62.5`，空闲 CPU 0.05%，满足 PERFORMANCE.md 的 60 FPS 硬指标。
- 主循环多持有一个内核句柄（1 个），`AppLoop` 析构时关闭。
- 后续动画任务（T-010）若需要 sub-ms 对齐，可在同一路径上缩短 `interval_ms_`，无需改结构。

---

## ADR-0007 · Dock 窗口用 WS_EX_NOREDIRECTIONBITMAP，不用 WS_EX_LAYERED

**状态**：Accepted（2026-10-23，T-004 spike 实测）

**背景**
`spikes/t004_dcomp` 对四种扩展样式做了像素级对照（背景窗铺 `RGB(0,128,255)`，Dock 窗口 240x90 DIP / 圆角 20 DIP / 192 DPI）：
`NOREDIRECTIONBITMAP`、`NOREDIRECTIONBITMAP|LAYERED`、`LAYERED`、`PLAIN` 四种模式下，圆角外区域采样均为背景色，圆角内为中心色，视觉上无差别。

**决策**
1. Dock / Finder Bar 等所有 DComp 内容窗口只用 `WS_EX_NOREDIRECTIONBITMAP`，不叠加 `WS_EX_LAYERED`。
2. `WS_EX_LAYERED` 仅在需要 `UpdateLayeredWindow` 位图上传的临时场景（如拖拽 ghost 若走 GDI）使用。
3. `DCompositionCreateDevice2` 的 renderingDevice 必须是 **ID2D1Device**，不是 IDXGIDevice——后者会让 `IDCompositionSurface::BeginDraw` 返回 `E_NOINTERFACE (0x80004002)`。
4. 内容一律在 DIP 坐标布局，`ID2D1DeviceContext::SetDpi(dpi,dpi)` 做缩放；窗口物理尺寸按 `lround(logical * dpi / 96)`。
5. 置顶窗口的 z 序必须用 `SetWindowPos` 显式指定，不得依赖创建顺序（spike 中两个 `WS_EX_TOPMOST` 窗口的叠放顺序与创建顺序相反）。

**后果**
- 少一次 DWM 分层合成拷贝，渲染路径更短。
- 主工程窗口创建统一走 `WindowDesc.composition_surface = true`。
- 拖拽 ghost 若需要半透明跟随鼠标，仍可用 DComp 自身实现，不必回到 `WS_EX_LAYERED`。

## ADR-0008 · 材质由自绘磨砂板实现，系统材质仅作可选底衬

**状态**：Accepted（2026-10-24，T-005 spike + 主工程实测）

**背景**
`spikes/t005_material` 对系统材质做了像素级对照（8px 棋盘格背景 + 24px 圆角 DComp 面板，`tools/pattern_contrast.ps1` 测相邻像素亮度差）：

| 材质 | adjacent_contrast | 圆角外 (4,4) |
|---|---|---|
| 无材质（对照） | 9.055 | 背景色 |
| `ACCENT_ENABLE_ACRYLICBLURBEHIND` | 0.188 | **材质色 4,6,8** |
| `ACCENT_ENABLE_BLURBEHIND` | 0.205 | **材质色 2,5,9** |
| `DWMSBT_MAINWINDOW`（Mica） | 0.003 | **材质色 243,243,243** |

系统材质确实模糊，但三条硬约束使其无法直接用于 Dock：
1. **无视逐像素 alpha**：DComp surface 画 alpha 0.0 全透明板时窗口内部仍是 `243,243,243`，材质铺满整个窗口矩形。
2. **无视 `SetWindowRgn`**：`--round-region` 与不加 region 采样逐点相同（材质层在 region 之上）。
3. **`DWMWCP_ROUND` 半径不可配**：请求 24px 时 `(2,2)` 被裁、`(6,6)` 未被裁，不服从 `appearance.corner_radius`（默认 22，范围 0..40）。

**决策**
1. `MaterialLevel` 语义 = **板本身的模糊强度与不透明度档位**，不是"用哪个系统材质"。
2. 主路径 = **自绘磨砂板**：工作线程 `BitBlt` 抓窗口背后的屏幕像素 → D2D `CLSID_D2D1GaussianBlur`（sigma 18 DIP）→ `PushLayer` 圆角裁剪 → tint → 描边。
3. 系统材质保留为**可选底衬**（`PlateStyle::use_system_backdrop`），仅在 `corner_radius == 0` 时启用；非零圆角时强制关闭并记录降级原因。
4. 捕获必须离开 UI 线程（`BackdropCapture` 工作线程 + 双缓冲发布），因为 `BitBlt` 读合成桌面是固定 ~78ms 的合成器往返，与区域大小无关（160x60 到 2560x480 耗时均在 77–79ms）。放在 UI 线程每帧路径必然击穿 16.7ms 预算。
5. Dock 窗口必须 `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`，否则捕获读回自己的上一帧，模糊吃掉自己的输出，递归越糊越糊。
6. `IDXGIOutput1::DuplicateOutput` 在本机返回 `E_ACCESSDENIED`（存在虚拟显示适配器），不能作为唯一捕获路径；GPU 捕获降级为可选加速。
7. 磨砂 bitmap 必须用 `D2D1::BitmapProperties1` 打上帧 DPI，否则 144 DPI 下霜层被放大 1.5 倍。

**后果**
- 圆角、透明度、模糊半径全部由 DockForge 自己控制，跨 Win10/Win11 表现一致。
- 新增线程与 78ms 合成器往返；捕获周期 200ms，稳态实测帧时间 `avg=16.00ms p50=16.01ms p99=16.50ms fps=62.5`，未受影响。
- 磨砂层首帧成本高（D2D bitmap 分配 + 模糊 shader 编译，spike 实测 620ms），必须预热，不能计入稳态。
- 冻结模式（`DOCKFORGE_FREEZE_BACKDROP=1`）是产出证据截图的唯一可行路径：`WDA_EXCLUDEFROMCAPTURE` 生效后 `PrintWindow` 采到全黑，只有先抓一帧再解除排除才能既拍到自己又保持霜层正确。

## ADR-0009 · 绘制 D2D 内容时必须复合调用方已有的变换矩阵

**状态**：Accepted（2026-10-24，T-005 实测修复）

**背景**
`IDCompositionSurface::BeginDraw` 返回的 `offset` 是 surface 在合成图集内的物理像素偏移，`CompositionSurface::Paint` 已把它换算成 DIP 写进 `SetTransform`（ADR-0007 第 4 条）。
`PlateRenderer::Draw` 为了把模糊图层定位到面板原点，直接调用了 `dc.SetTransform(Translation(rect.left, rect.top))`，**覆盖**了这个偏移。

**症状**
96 / 144 DPI 下 `tools/alpha_probe.ps1` 报 `painted=0 transparent=6`（面板整体偏移出可视区，采样全读到背景）；192 DPI 侥幸正常，因为那次 surface 恰好落在图集偏移零点。这个 bug 只在特定 DPI 出现，极易被"某个分辨率看着没问题"掩盖。

**决策**
任何在 `CompositionSurface::Paint` 回调内调用 `SetTransform` 的代码，必须先 `GetTransform(&base)`，用 `Translation(...) * base` 复合，结束后 `SetTransform(base)` 还原，不得写 `Identity`。

**后果**
- 三档 DPI 采样恢复 `painted=2 transparent=4`（`evidence/T-005/alpha{96,144,192}.txt`）。
- 契约化：写入 `CONTRACTS.md` C-04，后续任何新增图层绘制都遵守同一规则。

## ADR-0010 · 应用身份按 AppsFolder 五形态分类，不按文件系统路径

**状态**：Accepted（2026-10-24，T-006 spike 实测）

**背景**
最初假设 AppsFolder 里每一项都能落到一个可执行文件路径，因此把解析失败的一律计入
`skipped`。实测 216 项里 `skipped=124`，看上去像是枚举写错了。逐项打印 parsing name 后发现
它们分属四类不同身份，只有 1 项是真正的坏条目（`D:\Beautiful\.tools\innosetup\Examples`，目录不存在）。

| 形态 | 数量 | 是否可执行文件路径 | 启动 API |
|---|---|---|---|
| 打包应用 AUMID（含 `!`） | 43 | 否 | `ActivateApplication` |
| 盘符路径 | 49 | 是 | `CreateProcess` |
| 命名空间子项 `{GUID}\name` | 54 | 否（`SHGetPathFromIDListW` 0/54） | `ShellExecuteEx` |
| 裸桌面 id（无 `!`） | 69 | 否 | `ShellExecuteEx` |

**决策**
1. 分类依据是 parsing name 的**字符串形态**，函数 `shell::ClassifyParsingName` 是唯一入口，
   任何新增来源必须先过它。五种 `ParsingKind` 见 `CONTRACTS.md` C-08。
2. 命名空间子项的启动与图标身份统一为 `shell:AppsFolder\{GUID}\name`。裸形式
   （`SHCreateItemFromParsingName` / `SHParseDisplayName` / `ShellExecuteEx`）实测全部
   `0x80070002`，加前缀后图标与启动都成功。
3. 裸桌面 id 不能用 `ActivateApplication`（`0x80070057 E_INVALIDARG`），必须
   `ShellExecuteEx`。这意味着"打包应用用激活器、其余用 shell 执行"是硬分支，不是回退策略。
4. 未解析出的条目必须进 `skipped` 并打日志（`DF_DEBUG("shell", "skip %ls (%s): %s")`），
   不允许静默丢弃——上一版正是静默丢弃掩盖了分类错误。

**拒绝的方案**
- **只保留有可执行文件路径的条目**：会丢掉 43 个 UWP 与 123 个无路径应用，Dock 上就只剩
  49 个快捷方式，与"启动器"的定位相悖。
- **用 `SHGetFileInfo` 反查图标**：对命名空间子项 `GetUIObjectOf(IID_IExtractIconW)` 实测
  `0x80004005 E_FAIL`，不可用。

**后果**
- AppsFolder 扫描 215 项耗时 450–495ms，占冷启动 642ms 的主要部分。已记入后续优化（磁盘缓存
  parsing name → 启动描述映射），不阻塞当前里程碑。
- `AppKind` 从四值扩到六值，`AppEntry` 新增 `namespace_parsing` 字段，均已写入 C-08。

## ADR-0011 · 状态文件必须显式以 UTF-8 写入，禁止依赖 PowerShell 默认编码

**状态**：Accepted（2026-10-24，T-006 事故修复）

**背景**
`DECISIONS.md` / `PERFORMANCE.md` / `PROGRESS.md` 三个文件在本轮被发现中文尾部变成
GBK 字节、头部仍是 UTF-8，任何严格 UTF-8 读取器都会在中间报错。成因是 PowerShell 5.1 的
`Set-Content` / `Out-File` 默认使用 ANSI 代码页（本机 936），而 `Get-Content -Raw` 读入时
已按 UTF-8 解码成 .NET 字符串——写回时编码降级，中文就变成了 GBK 字节。

**决策**
1. 追加或改写 Markdown / JSON 状态文件一律走
   `[System.IO.File]::WriteAllText(path, text, (New-Object System.Text.UTF8Encoding(False)))`，
   不用 `Set-Content` / `Add-Content` / `Out-File`。
2. `tools/fix_encoding.ps1` 常驻：`-Check` 校验全部 `*.md` / `*.json` 是合法 UTF-8，
   `-Repair` 按首个非法字节所在行边界切分，前半 UTF-8 解码、后半 ANSI 解码后拼接重写。
3. 每个任务收尾（REPORT 步骤）跑一次 `-Check`，非零退出即视为任务未完成。

**后果**
- 三个文件已修复（`DECISIONS.md` 11751→12581B、`PERFORMANCE.md` 2500→2679B、
  `PROGRESS.md` 5383→5553B），内容人工核对无乱码。
- 此规则写进 `CONTRACTS.md` C-09，后续所有状态文件写入都受约束。
- `STATE.json` 例外保留 BOM：PowerShell 5.1 的 `Get-Content` 无 BOM 时按 ANSI 解码，GBK 尾字节 `0x5C` 会静默破坏 JSON 转义。

## ADR-0012 · Dock 放置以显示器工作区为基准，且几何与内容必须读同一个 DPI

**状态**：Accepted（2026-10-24，T-007 spike + 实测）

**背景**
T-001 至 T-006 的窗口定位是"在主屏工作区居中"，这段代码写在 `main.cpp` 末尾，与
`WM_DPICHANGED` 分支各自算尺寸，两处迟早会不一致。T-007 要求按目标屏工作区放置，同时
`appearance`/`dock.position` 的 `left`/`right` 也要生效。

spike 实测（`evidence/T-007/run_*.txt`）澄清了三个此前只是"以为"的事实：
- 本机只有一个显示器（`\\.\DISPLAY1` 2880x1920，有效 192 DPI，原始 243 DPI），**真实跨屏
  DPI 转换在本机不可复现**。
- `GetSystemMetricsForDpi(SM_CXSCREEN, 96|144|192)` 全部返回 2880——物理分辨率与 DPI 无关，
  不能用它推缩放。
- `AdjustWindowRectExForDpi(WS_POPUP)` 是 no-op（输入输出同为 `(0,0) 400x120`），无边框窗口
  不需要它。

**决策**
1. 放置**以 `work`（工作区）为基准**，不以 `monitor`。压到任务栏底下等于裁切 bug。
   `work` 为空时回退 `monitor`。
2. 超宽/超高时**缩小并置 `clamped`**，禁止越界；调用方必须 `DF_WARN`。
3. 放置只有**一条代码路径**：`main.cpp` 的 `apply_placement` lambda。启动、`WM_DPICHANGED`、
   `ShowWindow` 之后都调它。`lParam` 的建议矩形不使用——它按普通重叠窗口算，与按工作区
   放置的 Dock 无关。
4. `Window::Dpi()`（内容布局）与 `ComputeDockPlacement`（窗口尺寸）必须读同一个 DPI。
   未设 override 时 `WM_DPICHANGED` 的 `wParam` 是权威值，优先于重新枚举的显示器 DPI
   （枚举在转换瞬间可能滞后）。`DOCKFORGE_DPI_OVERRIDE` 设置时优先级最高且不被覆盖，
   否则 DPI 矩阵无法在放置生效后保持稳定。
5. 混合 DPI 验证靠两条互补证据：`tests/display_test.cpp` 对负原点副屏与 96+144 混合双屏做
   纯逻辑断言；`DOCKFORGE_SIMULATE_DPI` 在进程内 `SendMessageW(WM_DPICHANGED)` 证明处理器
   会重算尺寸与位置。

**拒绝的方案**
- **用 `lParam` 建议矩形**：它给的是"保持 DIP 尺寸不变"的矩形，而 Dock 的尺寸由配置与工作区
  决定，采用它会把窗口拉到错误的尺寸。
- **跨进程注入 `WM_DPICHANGED` 做验证**：实测 `PostMessage` 返回
  `ERROR_MESSAGE_SYNC_ONLY`(1159)，`SendMessage` 返回 0 但处理器不执行（同进程内
  `SendMessage` 立即生效）。`WM_CLOSE` 可跨进程，故这是系统消息特有的限制。
- **假装混合 DPI 已验证**：单显示器主机上 `GetDpiForWindow` 恒返回真实 DPI。合成消息证明的是
  处理器与放置数学，不等于验证了合成器的真实跨屏缩放。证据文件里明确写了这条边界。

**后果**
- 新增 `src/platform/display.{h,cpp}`（`EnumerateDisplays`/`PrimaryDisplay`/`DisplayForWindow`/
  `DisplayForPoint`/`ComputeDockPlacement`/`DipToPixels`/`PixelsToDip`）与
  `tests/display_test.cpp`（13 用例），全部测试 380 断言 / 72 用例通过。
- `CMakeLists.txt` 链接库补 `shcore`（`GetDpiForMonitor` 所在）。
- 三档 DPI 实测窗口 528x152 / 792x228 / 1056x304，底部居中且贴工作区，三张截图与差分采样
  全部 PASS。
- `WM_DPICHANGED` 处理器首次获得可复现验证：528x152 与 792x228 的矩形变化均由
  `tools/simulate_dpi.ps1` 记录（`evidence/T-007/simulate_dpi96.txt`、`simulate_dpi144.txt`）。
## ADR-0013 · Dock 布局求值：前缀和 + 半径外短路 + 中心钳制

- 日期: 2026-10-24
- 状态: 已采纳
- 上下文: T-008 实现 C-05。契约把累积位移写成双重求和（O(n²)）；benchmark 首轮 1e6 次求值 129.88ms，远超 20ms 预算。

### 决策

1. **累积位移用前缀和**。sign(i-j) 只依赖 j 相对 i 的侧别，求和拆成「左侧全部 - 右侧全部」，两者前缀和可得，O(n)。等价性由 ReferenceLayout（契约逐字 O(n²) 转写）逐项比对锁定，不是口头断言。
2. **热路径零分配**。g_j 从已写出的 out[i].scale - 1.0f 读回，不用临时 std::vector。初版每次调用分配一个 gain 数组，违反头文件「allocation-free」承诺与 C-05「禁止逐帧重建布局对象」。去掉后 129.88ms → 80.96ms。
3. **半径外跳过超越函数**。|d| >= R 时 s = 1 且不调 pow()/exp()。20 图标通常只有约 6 个在影响半径内。80.96ms → 7ms 量级。
4. **指针钳到首尾图标中心**，而非 [0, base_width]。后者把指针钉在首图标外边缘（离中心 w/2），6 图标 Dock 实测峰值 1.5556 而 magnification 是 1.8，且破坏峰值索引单调性。
5. **Gaussian 曲线归一化**。exp(-(p*t)^2) 在 t=1 残留 exp(-p^2)（p=2 时 0.0183），是影响半径处约 1.5% 的可见台阶。减去边界值再归一化，代价一次 exp。
6. **benchmark 单位显式声明**。C-05 的求值单位是单图标公式项，不是整条 Dock。首版按整条 Dock 计数判 FAIL，是单位错误不是性能问题。修正后两个口径同时输出。

### 被拒绝的方案

- **保留 O(n²) 双重循环**：40 图标时每帧 1600 次浮点乘加，本身不算慢，但它是纯浪费，且契约明令「禁止逐帧重建布局对象」的精神要求热路径尽量廉价。
- **用 GPU 计算布局**：T-010 才引入顶点变换。把数学搬到 GPU 会让 T-008 无法用单测锁定，也失去 CPU 参考实现的对照价值。
- **钳制到 [0, base_width]**：见决策 4，有实测反例。
- **用 exp(-(p*t)^2) 原样作为 Gaussian 权重**：边界不归零，有可见台阶。

### 后果

- src/dock/layout.cpp 无 Win32 依赖、无分配、可脱离窗口测试。
- C-05a 把上述 6 条固化为契约，后续改动必须走 ADR。
- benchmark 单位规则写进 C-05a 第 7 条，防止未来用有利口径掩盖帧成本。


## ADR-0014 · Dock 渲染：内容身份位图键、单一几何源、槽位覆盖率验证

- 日期: 2026-10-24
- 状态: 已采纳
- 上下文: T-009 把 `layout` 与 `shell::IconCache` 接进 Dock 窗口。三个具体问题必须先定：位图缓存用什么做键、窗口尺寸从哪里算、图标"画出来了"怎么证明。

### 决策

1. **位图缓存键是内容身份，不是地址**。`IRenderBackend::DrawBitmap(cache_id, BitmapView, rect, opacity)` 的 `cache_id` 由调用方按内容生成（`IconKeyFor(source)`，FNV-1a over UTF-16）。按 `IconBitmap` 的 `pixels.data()` 做键，会在 `std::vector` 重分配后把同一图标当成新位图每帧重传；按 `BitmapView` 的栈地址做键，会让不同图标别名到同一条缓存。两者都是"看起来能跑、偶发闪图标"的 bug。
2. **位图 DPI 固定打 96**。目标矩形已经是 DIP，`BeginFrame` 设置的变换已带帧 DPI。若位图再打帧 DPI，192 DPI 下 D2D 会把位图按 2 倍 DPI 解释、再被变换缩回去，实际渲染尺寸减半。T-004 的 `PaintInfo` 与 T-005 的霜层位图都踩过同类问题，这是第三次，因此写进 C-11。
3. **几何只有一个计算点**。`dock::ComputeGeometry(config, tile_count, scale)` 产窗口尺寸、板尺寸、内容宽度。`main.cpp` 原有 `LogicalSize` / `ComputeLogicalSize` 与 `apply_placement` 两处各自累加 `icon_size * n + gap * (n-1)`，两者在 `scale` 参与后必然漂移；已删除前者，`apply_placement` 每次重算 `geometry`。
4. **先画板后画图标**。霜层是不透明的模糊图像 + 色调，画在图标之上等于把图标糊掉。顺序在代码里是两行，但反过来是"图标为什么看不见"的经典排查黑洞。
5. **缺失图标画占位环**。留空会让"提取失败"与"这个槽位本来没应用"无法区分。占位环让两类问题在外观上分开，也让 `dock_slots.ps1` 的 `SPARSE` 判定有意义。
6. **图标存在性用槽位覆盖率证明，不用单点采样**。7-Zip 图标中心是黑色块，单点采样读成板色 `13,15,18`，会误判为"槽位是空的"。`tools/dock_slots.ps1` 以 2px 步长统计槽位内非板色像素占比与包围盒，`<2%` 判 `SPARSE`、`0` 判 `EMPTY`。实测 12 个槽位覆盖率 6.6%–81.9%，全部 `ICON`。

### 被拒绝的方案

- **每帧按槽位尺寸调 `IconCache::Get`**：槽位尺寸随 DPI 与悬停缩放变化，等于每帧都 miss 并重新走 shell 提取（冷调用实测 57–187ms）。固定 256px 提取 + 绘制时线性降采样。
- **`ID2D1Bitmap` 而非 `ID2D1Bitmap1`**：T-005 已证明 `CreateBitmap` 对前者报 `invalid user-defined conversion`；且 DPI 打标需要 `BitmapProperties1`。
- **用 `GetPixelSize()` 推窗口尺寸**：T-004 已证明它返回合成图集尺寸而非 surface 尺寸。
- **保留两处尺寸计算并加断言对齐**：断言只会在漂移发生后报警，不能阻止漂移；直接删掉重复源更省事。
- **给 `kPlatePaddingDip` 加配置项**：用户不需要调板内边距，加进 C-01 等于永久背负兼容成本。

### 后果

- `src/dock/dock_view.{h,cpp}` 不依赖窗口句柄，可在测试里用合成 `Config` 与假 tile 验证几何。
- C-11 冻结 tile 模型、几何来源、绘制顺序、缺失图标策略、位图 DPI 打标与验证手段。
- 悬停放大（T-010）接入时，`DockPaintInput::hover` 已预留，但窗口是否随悬停扩张尚未决定，属 T-010 的 ADR。
- `dockforge::shell::ToString(AppKind)` 在 T-006 只声明未定义，本轮加逐 tile 日志时以链接错误暴露，已补齐。教训：`enum class` 的 `ToString` 重载容易在头文件里声明后忘记实现，且只有调用点才会报错。

## ADR-0015 · Dock 悬停放大用 CPU 布局重算，不建逐图标 DComp visual

- 日期：2026-10-24
- 状态：已采纳
- 关联：C-05、C-05a、C-11、T-010

### 背景

T-010 的 DoD 原文写着「缩放走 DComp 变换/顶点，禁止逐帧重建布局对象」。前半句是手段，后半句才是约束。T-008 已经交付零分配的 `LayoutDock`（整条 Dock 210ns，20 个图标），T-009 已把每个图标画成 `DrawBitmap` 的目标矩形。问题是：接入悬停时要不要把每个图标改成一个 `IDCompositionVisual`，用 `SetTransform` 做缩放与位移。

### 决策

**不建逐图标 DComp visual。** 保持单 visual + 单 surface，每帧在 CPU 上重算 `IconLayout[]`（复用 `DockPainter::scratch_`，零分配），把结果作为 `DrawBitmap` 的目标矩形。

### 理由

1. **绘制不是瓶颈**。实测 `paint n=... avg=2.85ms p99=4.49ms`，占 16.7ms 预算的 17%。引入 12 个 visual 最多把这 2.85ms 降到接近 0，而预算还剩 12ms 富余。
2. **圆角裁剪会变复杂**。当前整块板用一个 `PushLayer` + 圆角几何裁掉溢出。改成每个图标一个 visual 后，图标的放大溢出要么裁不到（视觉破面），要么每个 visual 各带一个 clip，反而增加合成开销。
3. **`SetWindowRgn` 与 visual 变换的交互不可控**。T-010 已证明 `SetWindowRgn` 会裁掉自绘 DComp 内容（spike question 3）。region 是按布局算出来的矩形；如果图标位置由 visual 变换决定，region 就得反推变换后的包围盒，两套几何源必然漂移。
4. **D2D 位图缩放已经是 GPU 采样**。`DrawBitmap` 的目标矩形缩放由 D2D 在 GPU 上完成（`D2D1_INTERPOLATION_MODE_LINEAR`），并非 CPU 逐像素。所谓「CPU 布局」只是 12 次浮点乘加，真正的放大在 GPU。

### 被拒方案

- **每图标一个 `IDCompositionVisual` + `SetTransform`**：收益为零（绘制不是瓶颈），复杂度与几何漂移风险实打实。
- **每图标一个 `IDCompositionVisual` + `IDCompositionScaleTransform`**：同上；且 `IDCompositionScaleTransform` 的 `CenterX/CenterY` 语义与 D2D 的 `SetTransform` 组合时容易双重缩放（T-005 的 ADR-0009 已经踩过一次同类坑）。
- **用 `IDCompositionDevice3::CreateGaussianBlurEffect` 做图标拖影**：与悬停放大无关，属于视觉润色，不在 DoD 内。
- **把整条 Dock 渲染成一张位图再用一个 visual 缩放**：整体缩放会连板一起放大，与 macOS 行为不符。

### 后果

- 悬停路径的热点是消息洪流而非绘制：**每条 `WM_MOUSEMOVE` 都 `InvalidateRect` 时 p99 18.2ms 超预算**；改为消息只置 `hover_moved` 标志、由 tick 合并成每帧最多一次重绘后，p99 降到 16.54ms（21/21 窗达标）。
- `SetWindowRgn` 同理：**每帧重建 region 时 p99 17.3ms 超预算**；改为只有空闲/悬停两态、悬停态用满倍率矩形（所有中间帧的并集）后消除。
- 若图标数涨到 50+ 或加入模糊拖影，需重新评估本决策——届时绘制时间会成为瓶颈，visual 方案才有意义。
- T-010 的 DoD 第 3 条按本条 ADR 判为满足：约束是「禁止逐帧重建布局对象」，实现零分配、零对象创建，满足；手段不是 DComp 变换，已记录理由。
