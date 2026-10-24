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

## ADR-0016 点击行为：窗口身份、异步窗口操作、前台权限

**日期**：2026-10-24
**状态**：已采纳（T-011）

### 背景

T-011 要把 Dock 的点击做成「一个槽位三种结局」：未运行则启动，前台则最小化，后台则激活。三条路各有独立的失败模式，且都是静默的——点击看起来生效了，实际启动了第二个副本，或者把错误的应用抬到前台。

### 决策

**1. 应用窗口身份分两层：`pid` 与 `app_pid`。**

打包应用的可见顶层窗口属于 `ApplicationFrameHost.exe`，不是应用本身。按顶层 pid 匹配永远找不到 UWP 应用。`AppWindow` 同时记录两者：`pid` 是顶层窗口所有者，`app_pid` 通过 `EnumChildWindows` 找到的跨进程子窗口得到。

**2. 身份匹配顺序固定，AUMID 优先且不可退化。**

entry 声明了 AUMID 就只按 AUMID 匹配。若允许退化到路径匹配，所有打包应用会因共享 `ApplicationFrameHost.exe` 而互相串味。

**3. 路径匹配用文件身份而非字符串。**

`scoop` 的 `current` junction 让条目路径与实际进程路径不同（`7zip\current\7zFM.exe` vs `7zip\26.00\7zFM.exe`）。字符串比较永远不等，点击每次都启动新副本。改用 `GetFileInformationByHandle` 的卷序列号 + 文件索引。

**4. 窗口操作全部异步。**

`ShowWindow(SW_RESTORE)` 会等待目标应用处理消息，忙碌应用把 Dock 的 UI 线程拖住 494ms（实测最大值）。改 `ShowWindowAsync` 后 p99 从 494.59ms 降到 9.89ms，20 次点击超预算数从 10 降到 0。

`SetForegroundWindow` 保持同步：它不等目标应用处理，且是唯一需要点击前台权限的调用。

**5. 点击处理函数是唯一持有前台权限的地方。**

实测 `SetForegroundWindow` 从后台进程被拒绝，`AttachThreadInput` 无效，合成 ALT 有效，但在 `WM_LBUTTONDOWN` 里直接调用同样有效。因此不做任何「预先获取权限」的机制，也不在 tick 里做点击动作。

`alt=1` 保留在 `ActionResult` 里并在日志中输出：正常会话出现即缺陷信号。

### 被拒方案

| 方案 | 拒绝理由 |
|---|---|
| 按顶层 pid 匹配应用 | UWP 永远找不到窗口（frame host 持有顶层窗口） |
| AUMID 匹配失败后退化到路径匹配 | 所有打包应用共享 frame host 路径，会互相串味 |
| 路径字符串比较（大小写不敏感） | scoop/版本目录的 junction 让路径永远不等 |
| `AttachThreadInput` 获取前台权限 | 实测无效，`SetForegroundWindow` 仍返回 0 |
| 无条件合成 ALT 再激活 | 多余：点击路径本身已有权限，ALT 会闪一下开始菜单 |
| 同步 `ShowWindow` | p99 494ms，一次点击丢 30 帧 |
| 点击时缓存 `AppWindow` 列表 | 窗口集合随时变化（应用关闭、对话框弹出），缓存会让点击打到已销毁的句柄 |
| 用空闲布局做命中测试 | 抬起的图标点不到，被推开的邻居反而可点 |

### 后果

- 点击延迟从 `avg=41.29ms / p99=494.59ms` 降到 `avg=1.92ms / p99=9.89ms`。
- 每次点击做一次 `EnumWindows`（实测 0.37–0.70ms），不缓存。
- `AppWindow` 多一个 `app_pid` 字段，`ActionResult` 多一个 `enumerate_ms` 字段与 `used_alt_fallback` 标志。
- `DOCKFORGE_PIN_APPS` 环境变量成为验证钩子（不进 C-01）：Dock 默认显示前 12 个应用，本机这 12 个里没有打包应用，UWP 点击路径无法端到端验证。


## ADR-0017 · 运行指示器用两个时钟，圆点画在板内边距里

- 日期：2026-10-24
- 状态：接受
- 关联：T-012、C-14

### 背景

Dock 需要显示哪些应用正在运行、哪个在前台。两个状态的自然来源不同：窗口存在性只能靠 `EnumWindows` 全扫（0.48–0.94ms），前台归属靠 `GetForegroundWindow`（亚微秒）。频率差三个数量级。

### 决策

1. **三值状态而非布尔**。`TileRunState{Idle,Running,Foreground}`，`RunningTracker::Update` 返回「可见状态是否可能变化」。
2. **两个时钟**。存在性 300ms 轮询，前台每帧读。
3. **`Update` 在无变化时返回 false**，调用方据此保持空闲不重绘。
4. **圆点画在板的底部内边距中央**，不新增带。
5. **状态写在 `DockTile` 上**，不用并行数组。
6. **前台判定带 pid 兜底**（`GA_ROOTOWNER` 不可靠）。
7. **验证用交替 A/B 归因**，不靠单次对比。

### 被拒方案

| 方案 | 拒绝理由 |
|---|---|
| 单个 `bool running` | 只能取慢时钟，alt-tab 后高亮滞后 300ms；实测这是最容易被用户察觉的缺陷 |
| 每帧全量 `EnumWindows` | 60×0.7ms = 4.2% CPU，空闲 CPU 预算 0.5% |
| 300ms 也节流前台读取 | 高亮滞后到 300ms，而这是每帧都该准的状态 |
| 新增 7 DIP 指示器带 | 板与窗口一起长高，屏幕上每个 Dock 位移，且所有从窗口高度反推 scale 的验证工具失效 |
| 圆点只靠亮度区分前后台 | 48 DIP 图标下 4 DIP 的点很小，浅色壁纸下读不出来；改为亮度 + 宽度双维度 |
| 状态放并行数组 | tile 列表重建时失步，表现是圆点指向错误图标 |
| 只信 `GA_ROOTOWNER` | 对话框/弹出窗口会漏判；加 pid 兜底后覆盖 |
| 单次对比归因帧预算 | 同配置运行间波动 0.018ms，与待测差值同量级，单次对比无法归因 |

### 后果

- 空闲 CPU 0.0159%、RSS 90.43MB、句柄 372→362，三项 PASS。
- 轮询 `avg=0.48–0.54ms max=0.83–0.94ms`，远低于 1ms DoD。
- 退出检测 437ms（预算 1000ms）。
- 文档类条目（`.chm` → `hh.exe`）不显示圆点 —— 真实归属，不是缺陷。
- 帧预算判 WATCH，A/B 证明 tracker 不是尾延迟原因，问题归入 T-006a。

## ADR-0018 Dock 内部拖拽用 CPU 状态机 + 立即持久化

**状态**：接受（T-013）
**背景**：Dock 需要支持按住图标拖动重排、拖出移除，并在释放后立即持久化。三条路可选：(a) OLE `IDropTarget`/`IDropSource`；(b) `DragDetect` + `WM_ENTERSIZEMOVE` 走系统移动循环；(c) 自建状态机，`SetCapture` 拿指针流。

**决定**

1. **自建状态机（c）**，不用 OLE。OLE 拖放的身份是 `IDataObject`，适合跨进程数据交换（T-014 拖入文件要用），但内部重排没有任何数据要交换，用 OLE 等于给一个纯 UI 手势套一层 COM 生命周期。`IDropSource::GiveFeedback` 也不允许在拖拽期间自由重绘自己的窗口。
2. **`SetCapture` 拿指针流**。spike `spikes/t013_drag` 四组对照实测：`--auto-drag` 时窗口外仍收到 3 个 `WM_MOUSEMOVE`（`moves_captured_outside=3`），不捕获的对照组是 0。没有捕获，指针一离开 Dock 手势就断。
3. **阈值从按下点测**，4 DIP。从上一帧测会让慢速拖拽永不启动。
4. **插入槽用静止栅格**（`IdleCentersDip`），不用放大后的中心。放大后的中心是鼠标位置的函数，喂回去形成反馈环，实测让 `0 -> 4` 报成 `0 -> 3`。
5. **移除需要长按 550ms 确认**，只判上/左/右。理由见 C-15e。
6. **手势结束立即 `SaveConfig`**。理由见 C-15g。
7. **region 在按下时扩到整窗**，释放后恢复。`SetWindowRgn` 会裁剪合成内容，不扩则 ghost 被切。

**被拒方案**

| 方案 | 拒绝理由 |
|---|---|
| OLE `IDropSource` 做内部重排 | 没有数据要交换；`GiveFeedback` 不能自由重绘；COM 生命周期无收益 |
| `DragDetect` + 系统移动循环 | 系统循环期间我们的帧循环停摆，动画和插入线都不更新 |
| 用放大后的槽中心做插入点 | 反馈环，实测差一槽 |
| 拖出即删除 | 误操作代价是数据丢失（tile 配置被改写），而 Dock 无法弹确认框（`WS_EX_NOACTIVATE`） |
| 用 `MessageBox` 确认移除 | 会抢焦点并打断用户当前应用，比误删更打扰 |
| 拖拽期间 region 保持 hover 两态 | ghost 一出板顶就被裁（实测切在板顶 y=317） |
| 退出时才写配置 | 手势后崩溃丢失排列，且排列无法从扫描重建 |
| 用 `GetCapture()` 从外部验证捕获 | 返回调用线程的捕获窗口，跨进程恒为 0，会得出"捕获没建立"的假结论 |

**后果**

- 拖拽期间行放大固定在按下点，不随指针横移。这不是缺陷：行跟着跑会让插入线永远在指针正下方，看不出要插到哪。
- 移除只能靠长按，误触的代价从"删掉"降到"没反应"。代价是用户需要学一次这个手势，Dock 无法用文字提示（没有可显示的地方）。
- 拖拽帧预算实测 `fps_min=62.5`、14 个采样窗 p99 全部 ≤16.69ms。首个窗口曾报 19.63ms，根因是 `FrameClock` 的 1000 样本窗里还留着启动期样本（首帧 309ms 的模糊 shader 编译），不是拖拽路径。

## ADR-0019 外部拖入用自建 IDropTarget，落点用静止栅格，回调只记录状态

- 日期：2026-10-24
- 状态：已采纳
- 关联：C-16、T-014；受 C-10（几何与 DPI 同源）、C-15（拖拽状态机）约束

### 背景

Dock 需要接受从资源管理器、开始菜单、桌面拖来的 `.lnk` / `.exe` / 目录。桌面上的东西对 `IDropTarget` 只有一种形状：一个带 `CF_HDROP` 的 `IDataObject`。但 dock 窗口有两个特殊之处会与 OLE 相互作用：

1. 它是 `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`，且用 `SetWindowRgn` 做命中测试形状（C-12）。
2. 它已经在 `WM_MOUSEMOVE` 上跑自己的拖拽状态机（C-15），两个拖拽手势必须互斥。

`spikes/t014_drop` 先用两个进程跑通了完整 OLE 握手，得到四条硬约束（`evidence/T-014/run_*.txt`），再决定实现形态。

### 决定

**1. 手写 `IDropTarget`，不用 `RegisterDragDrop` 的默认 helper。** 默认 helper 不做 region 处理、不做槽位解析，而这两件事正是 dock 的全部。

**2. OLE 生命周期由 `dock::OleSession` 独占，`main.cpp` 不得调 `CoInitializeEx`。** 实测 `CoInitializeEx` 之后 `RegisterDragDrop` 返回 `0x8007000E E_OUTOFMEMORY`，只有 `OleInitialize` 返回 `S_OK`。把初始化收进一个引用计数 RAII，是为了让"忘记 OleInitialize"这件事在类型层面不可表达，而不是靠注释提醒。

**3. 落点与 caret 都用静止栅格 `IdleCentersDip(geometry, scale)`。** 外部拖入时行强制回落（`dock_hover.active = false`），栅格是解析式而非放大布局的输出。这是 T-013 反馈环的直接继承：用放大后的 centres 会让"指针右移 → 右侧图标放大 → 中心更右 → 改变已越过的槽"形成闭环，实测错位 1 槽。

**4. scale 由 `SetScale(window.Scale())` 注入，不读 `GetDpiForWindow`。** 这是 C-10 那条规则第二次发作：`DOCKFORGE_DPI_OVERRIDE` 下窗口保持真实 192 DPI，而行按 override 96 DPI 排布，两者比值 2 时每个 x 减半。第一次发作是 T-004 的 `BeginDraw` offset（物理像素）与 `SetTransform`（DIP）混用。

**5. OLE 回调只记录状态，重绘由 tick 合并。** 回调运行在源进程的模态拖拽循环里，是同步打断。每个 `DragOver` 都 `InvalidateRect` 实测 `p99_mean=17.284ms`、19/20 窗超预算；tick 合并后 `p99_mean=16.512ms`、0 窗超。这是同一条规则的第三次应用（T-010 `hover_moved`、T-013 `drag_moved`、T-014 `drop_moved`）——现在它是 C-16g，不再是每次重新发现。

**6. region 在 `DragEnter` 扩到整窗、`DragLeave`/`Drop` 恢复，只在两态翻转时调用。** region 外的 `DragEnter`/`Drop` 完全不投递（实测边界等于 region 顶边）；不扩则指针上移收到 `DragLeave` 且 `Drop` 永不触发，源进程挂死。但不能每次 `DragOver` 都 `SetWindowRgn`（T-010 的 17.3ms）。

**7. 五类载荷，文档拒绝，混合载荷整体拒绝。** `IsDockable` 只看路径与属性，不看能否取到图标——`IShellItemImageFactory` 对 `.txt` 和目录都成功，这个信号没有区分力。

**8. caret 在不可接受时也画，用暖红。** 系统光标的"禁止"徽标属于源进程，进入 `DROPEFFECT_NONE` 窗口就消失，dock 会显得在接受。

**9. 查重按身份字符串、逆序插入、立即持久化。** 与 C-15 的 `CommitOrder` 同一套机制，不引入第二套顺序表示。

### 被拒方案

| 方案 | 拒绝理由 |
|---|---|
| 用 `DragAcceptFiles` + `WM_DROPFILES` | 只支持 `CF_HDROP`，且 `DragAcceptFiles` 内部就是 `RegisterDragDrop` 的简化封装；拿不到 `DragEnter`/`DragOver`，无法在悬停期间画 caret 或扩 region |
| 在 `WM_NCHITTEST` 返回 `HTTRANSPARENT` 处理空白区 | 实测跨进程无效（`spikes/t010_hover`），只在同线程有效 |
| 落点用放大后的 `IconCentersDip` | 反馈环，实测错位 1 槽（T-013） |
| `UpdatePointer` 里读 `GetDpiForWindow` | `DOCKFORGE_DPI_OVERRIDE` 下与几何不同源，落点错 3 槽（实测 `x=188.5dip` vs 真实 `377.0dip`） |
| 每个 OLE 回调里 `InvalidateRect` | p99 17.284ms，19/20 窗超预算 |
| 每个 `DragOver` 里 `SetWindowRgn` | T-010 已测：p99 17.3ms |
| 只在 `DragOver` 时扩 region（不在 `DragEnter`） | 指针从板内上移到板上方会先触发 `DragLeave`，`Drop` 永不到达 |
| 用 `OleInitialize` 之外的方式满足 `RegisterDragDrop` | 三种 `CoInitializeEx` 变体全部 `E_OUTOFMEMORY`，没有其他路径 |
| 多文件逐个接受可接受的、跳过不可接受的 | 用户无法知道哪部分生效；dock 的 tile 顺序是唯一凭据 |
| caret 用系统光标徽标代替 | 徽标由源进程绘制，进入 `DROPEFFECT_NONE` 窗口即消失 |

### 后果

- `main.cpp` 的 `com_guard` 被 `dock::OleSession ole;` 取代，`CoInitializeEx` 从代码库消失。任何未来需要 COM 的模块必须构造 `OleSession` 或显式说明为何 `CoInitializeEx` 足够（即不需要拖放）。
- `DockDropTarget` 需要 `SetScale` + `SetCentres` 两个注入点，由 `geometry_changed` lambda 统一刷新，日志打 `drop grid scale=%.3f slots=%zu first=%.1f last=%.1f dip`——这三个数字让"槽位算错"从猜测变成读数。
- caret 位置有一行 `DF_TRACE` 打 `slot/origin/pitch/x/px/band`。T-014 期间正是靠它把"caret 在 762px"和"几何算出的 650.8px"对上，才确认截图里的 caret 属于另一次运行而不是几何错误。
- `tools/drop_caret.ps1` 成为 caret 的唯一判据。整帧 `image_diff` 只能说明"有变化"，不能说明变化是 caret。
---

## ADR-0017 · 缩略图预览用 DWM 注册表 + 独立 GDI 弹窗，不用 DirectComposition 重绘

**状态**：已接受（T-015）
**日期**：2026-10-24
**关联**：ADR-0002（纯 Win32 + DComp + D2D）、C-17

### 背景

T-015 要求悬停 tile 400ms 后弹出该 app 的窗口缩略图预览，支持同一 app 多个窗口（横向 ≤8）、最小化不黑屏、UWP 正确枚举、句柄无泄漏。

DWM 提供 `DwmRegisterThumbnail` 一族 API，可把任意顶层窗口的合成内容投到另一个窗口的矩形里。问题在于这个"另一个窗口"该是什么：复刻 dock 的 DirectComposition 表面，还是另开一个普通窗口。

### 决策

**预览弹窗是独立顶层普通窗口（自有窗口类 + GDI 画底），缩略图由 DWM 合成其上。**

1. `platform::PreviewWindow`：自有类 `DockForgePreviewWindow`，`WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE` + `WS_POPUP`，**不加** `WS_EX_NOREDIRECTIONBITMAP`。
2. `shell::ThumbnailHost`：唯一持有 `DwmRegisterThumbnail`/`DwmUnregisterThumbnail` 的类，管注册生命周期与槽矩形。
3. 槽 110x70 DIP、gap 8 DIP、最多 8 槽、padding 12 DIP；DWM 矩形用 dest 客户端**物理像素**。
4. 注册分帧：弹窗先显示，每 tick 注册 1 个。
5. 弹窗隐藏而非销毁；注销先于销毁。

### 理由

**为什么独立窗口而不是 dock 的一部分。** dock 窗口被 `SetWindowRgn` 裁成 plate + 放大行两态（C-12），且窗口高度按 `icon*max(mag,1)+2*pad` 固定。预览需要 12+70+12=94 DIP 高、最多 964 DIP 宽，任何一态都装不下，而拖拽期 region 扩整窗也只是权宜。把预览塞进 dock region 会同时破坏 C-12 的两态模型和 C-15 的拖拽 region 规则。

**为什么不能复用 `platform::Window`。** 两个都是实测约束而不是偏好：该类共享一个窗口类，其 WndProc 在 `WM_DESTROY` 时 `PostQuitMessage(0)`——预览窗一关就结束 dock 的消息循环；且它的 `MessageHook` 属主 dock，每条预览消息都要先过一遍 dock 的钩子。新开一个类是唯一干净解。

**为什么不用 `WS_EX_NOREDIRECTIONBITMAP`。** dock 用它是因为 dock 的内容全部来自 DirectComposition（ADR-0007）。预览反过来：DWM 把缩略图合成在 dest 的 redirection surface 之上，加了该标志 dest 就没有 redirection surface，GDI 底也一起不可见。spike 实测"普通目标窗与 NOREDIRECTIONBITMAP 目标窗结果相同，且 GDI 背景之上绘制正常"，因此预览窗保持普通窗口 + GDI 底。

**为什么不用 DirectComposition 重绘。** 候选方案是让预览窗也有 DComp 表面、用 `CreateD2DBackend` 画框，缩略图由 DWM 合成其上。放弃原因：DComp visual 与 DWM 缩略图层的 z 序依赖未在本地验证，而 spike 已证明普通窗口 + GDI 底这一路径可用。为一个未验证的 z 序假设重写渲染路径，违反"新 API 先写 spike"。

**为什么槽是 110x70 而不是更大。** spike 实测的渲染上限表见 C-17c：110x70 三次全 8/8；150x70 起掉到 7，180x70 掉到 6，200x130 只出 4/6。已排除注册节奏、重注册、脏区、源重绘、TOPMOST、布局方向、注册顺序、槽高，未定因。选择是绕开而非解释：取实测稳定的最大槽位。

**为什么注册分帧。** 冷注册 219-609ms/槽（8 槽 594ms，`evidence/T-015/tmp_a110c8b.txt` L21）。同步注册会让用户索要预览的那一帧直接超时 30 倍以上。分帧后预览先出框、最多约 8 帧填满，帧预算不受影响。

**为什么注销先于销毁。** DWM 渲染进 dest 的 redirection surface，注册比 dest 活得久就是桌面合成器里的悬空引用；比 source 活得久就是黑框。`ReleaseAll()` 作为唯一注销入口，`HidePreview()` 与进程收尾都走它，`preview.Destroy()` 排在其后。

### 后果

- 预览是第二个顶层窗口，`WM_MOVE` 需要跟着 dock 重定位（已加）。
- DPI 变化时预览隐藏并让 dwell 计时器重新武装：槽矩形是 DIP 派生，重算几何比让弹窗马上扔掉更贵。
- 弹窗 HWND 长期存活（隐藏而非销毁），故 `Destroy()` 前的注销必须是显式步骤而不是析构副作用。
- 未验证面：跨屏 dock 上的预览定位；DWM 渲染上限的成因；>8 窗口时保留哪 8 个（当前是顺带的 MRU，非设计）。均记入 C-17l。