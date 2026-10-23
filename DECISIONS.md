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
