# CONTRACTS.md · 冻结契约

> 变更规则：任何跨模块接口一经写入本文件即冻结。修改必须新增 ADR（DECISIONS.md）并显式迁移（记录迁移任务 ID）。

---

## C-01 配置 Schema v1

配置文件：`%APPDATA%\DockForge\config.json`（UTF-8，JSON）

| 键 | 类型 | 默认 | 范围/枚举 | 单位 |
|---|---|---|---|---|
| dock.icon_size | int | 48 | 32..96 | px@100%DPI |
| dock.gap | int | 8 | 0..32 | px@100%DPI |
| dock.magnification | float | 1.8 | 1.0..2.5 | 倍数 |
| dock.hover_falloff | enum | sine | sine/cosine_integral/gaussian | - |
| dock.influence_radius | float | 3.0 | 1.0..5.0 | 图标宽倍数 |
| dock.falloff_sharpness | float | 2.0 | 0.5..4.0 | 指数 p |
| dock.position | enum | bottom | bottom/left/right | - |
| appearance.material | enum | mica | mica/acrylic/blur/solid | - |
| appearance.corner_radius | int | 22 | 0..40 | px@100%DPI |
| behavior.autostart | bool | false | - | - |
| behavior.hide_taskbar | bool | false | - | - |

**实现约束**
- 未知键：忽略但保留（写回时不得丢失）。
- 越界值：钳制到范围并在日志 WARN，不抛异常。
- 解析失败：整份回退默认值，保留损坏文件为 `config.json.bad`。

## C-02 主题包

```
themes/<name>/theme.json + preview.png(512x288) + icons/
theme.json: { name, author, version, extends, overrides:{<C-01 子集>} }
```

## C-03 目录约定

- 配置：`%APPDATA%\DockForge\config.json`
- 主题：`%APPDATA%\DockForge\themes\`
- 日志：`%LOCALAPPDATA%\DockForge\logs\dockforge-YYYYMMDD.log`
- 转储：`%LOCALAPPDATA%\DockForge\dumps\`

## C-04 渲染后端契约（T-004 冻结，T-005 扩充）

- 后端接口 `dockforge::render::IRenderBackend`，由 `src/render/backend.h` 定义。
- 所有坐标在**逻辑像素**（DIP）下计算，paint 时由后端乘以 `scale`。
- 后端必须在 `resize(w,h)` 后一帧内清屏，不得残留上一帧内容。
- 方法名用 `DrawLabel` 而非 `DrawText`：`<winuser.h>` 把 `DrawText` 定义为 `__MINGW_NAME_AW` 宏，成员函数名会被文本替换成 `DrawTextW`。
- 笔刷按 RGBA 精确缓存（16 槽），槽满时用临时笔刷溢出，**不淘汰已有条目**；`BeginFrame` 只在 `ID2D1DeviceContext` 实例变化时清空缓存（笔刷是设备相关的）。

### C-04a 变换矩阵复合规则（T-005 冻结，ADR-0009）

`CompositionSurface::Paint` 回调内的 `ID2D1DeviceContext` 已带有一个平移矩阵（surface 在合成图集内的偏移，见 ADR-0007 第 4 条）。任何图层绘制**必须**：

```cpp
D2D1_MATRIX_3X2_F base{};
dc.GetTransform(&base);
dc.SetTransform(D2D1::Matrix3x2F::Translation(local_x, local_y) * base);
// ... 绘制 ...
dc.SetTransform(base);   // 还原，禁止写 Identity
```

违反此规则的症状是特定 DPI 下内容整体偏移出窗口（96/144 DPI 采样全透明，192 DPI 侥幸正常）。

### C-04b 材质契约（T-005 冻结，ADR-0008）

- `core::MaterialLevel` 枚举值 `Mica / Acrylic / Blur / Solid` 表示**板自身的模糊强度与不透明度档位**，不表示"使用哪个系统材质"。
- 降级链固定为 `Mica → Acrylic → Blur → Solid`，由 `render::ResolveMaterialTier(requested, caps)` 单向向下走，不得跳过中间档位。
- 系统材质（`DWMWA_SYSTEMBACKDROP_TYPE` / `SetWindowCompositionAttribute`）只在 `PlateStyle::use_system_backdrop == true` 时启用，且该标志在 `corner_radius != 0` 时恒为 false。
- `render::DegradeReason(requested, caps)` 返回单条最紧的约束文案，未降级返回空串；降级必须同时写 `DF_WARN`。
- 环境变量 `DOCKFORGE_FORCE_MATERIAL=<mica|acrylic|blur|solid>` 覆盖配置里的材质请求（测试钩子，不写入配置文件）。
- 自绘磨砂板依赖屏幕捕获，`MaterialCaps::capture_available == false` 时 Blur 档必须降级到 Solid，不得黑屏。
- 捕获线程与 UI 线程之间只传**拷贝**（`BackdropCapture::AcquireIfNew` 填充调用方缓冲），不传裸指针。

## C-05 Dock 布局数学（T-008 冻结）

对图标 i（中心 x 坐标 c_i，宽度 w），鼠标 x = m：

```
d_i = |m - c_i|
R   = influence_radius * w
t   = clamp(d_i / R, 0, 1)
s_i = 1 + (magnification - 1) * (1 - t)^falloff_sharpness   // 简化锐度曲线，见 ADR-0004
x_i = base_i + Σ_{j} (s_j - 1) * w * 0.5 * sign(i - j)
```

- 缩放与位移**只在顶点缓冲/变换矩阵中体现**，禁止逐帧重建布局对象。
- 帧时间预算 p99 < 16.7ms。

### C-05a 求值规则（T-008 冻结，实现细节不可自由发挥）

1. **指针钳制**：m 先钳到首尾图标**中心**的跨度 [w/2, base_width - w/2]（base_width 为无悬停内容宽度）。钳到 [0, base_width] 会让边缘图标峰值达不到 magnification，并破坏峰值索引的单调性。
2. **累积位移用前缀和**：x_i = base_i + 0.5*w*(Σ_{j<i} g_j - Σ_{j>i} g_j)，g_j = s_j - 1。与契约双重求和代数等价，tests/layout_test.cpp 的 ReferenceLayout（逐字 O(n²) 转写）逐项比对锁定，容差 scale 1e-5 / center 1e-3 / width 1e-4。
3. **零分配**：热路径 LayoutDock(params, count, mouse_x, hovering, IconLayout* out, capacity, summary) 禁止任何堆分配。g_j 从 out[i].scale - 1.0f 读回，不用临时数组。
4. **半径外短路**：|d| >= R 的图标 s = 1 且跳过 pow()/exp()。ComputeScale 本身也必须对 d >= R 返回恰好 1.0f、对 d <= 0 返回恰好 magnification。
5. **曲线在影响半径处必须归零**：FalloffWeight(1.0, params) < 1e-6。Gaussian 必须归一化 (exp(-(p*t)^2) - exp(-p^2)) / (1 - exp(-p^2))，未归一化在 p=2 残留 0.0183（≈1.5% 缩放台阶）。
6. **magnification < 1 钳到 1**，不允许缩小。
7. **求值单位**：benchmark 的「一次求值」= 单个图标的 C-05 公式项，不是整条 Dock。两者都要输出，禁止只报有利口径。

## C-06 模块边界

| 模块 | 职责 | 不得触碰 |
|---|---|---|
| src/platform | Win32/DWM/Composition 封装、DPI、窗口类注册 | 业务状态 |
| src/render | 材质、模糊、圆角、动画曲线、绘制后端 | 窗口消息循环 |
| src/core | 配置、日志、事件总线、状态机 | 直接 GDI/D2D 调用 |
| src/shell | 图标提取、.lnk 解析、窗口枚举、缩略图 | UI 绘制 |
| src/dock | Dock 布局与交互 | 直接 P/Invoke 之外的系统调用 |
| src/finderbar | 顶部栏 | Dock 状态 |

## C-07 日志格式（T-002 冻结）

```
<ISO8601 本地时间带偏移> <LEVEL> [<module>] <message>
例：2026-01-01T12:00:00.123+08:00 INFO  [core] config loaded from ...
```
LEVEL ∈ TRACE/DEBUG/INFO/WARN/ERROR/FATAL。文件按天切分，保留 7 天。

## C-08 shell 应用解析契约（T-006 冻结）

### 身份五形态

`AppsFolder` 里能枚举到的每一项，其 `SHGDN_FORPARSING` 字符串必属下列之一。分类函数是
`shell::ClassifyParsingName`，它是扫描器唯一入口，任何新增来源都必须先过它。

| ParsingKind | 形态 | 样例 | 启动方式 |
|---|---|---|---|
| `ShortcutPath` | `.lnk` 绝对路径 | `C:\...\AutoHotkey Dash.lnk` | `IShellLinkW` 解析 → `CreateProcessW` |
| `FilePath` | 可执行文件绝对路径 | `C:\msys64\mingw64.exe` | `CreateProcessW` |
| `PackagedAppId` | 打包应用 AUMID（含 `!`） | `Microsoft.WindowsCalculator_8wekyb3d8bbwe!App` | `IApplicationActivationManager::ActivateApplication` |
| `DesktopAppId` | 无 `!` 的裸应用 id | `Anysphere.Cursor`、`Chrome` | `ShellExecuteExW("shell:AppsFolder\<id>")` |
| `NamespacePath` | `{GUID}\name` 命名空间子项 | `{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\cmd.exe` | `ShellExecuteExW("shell:AppsFolder\{GUID}\name")` |

对应 `AppKind`：`Shortcut` / `Executable` / `Uwp` / `ShellItem` / `NamespaceItem`（+`Unknown`）。

### 三条实测硬约束（违反即失败）

1. **命名空间子项必须先加 `shell:AppsFolder\` 前缀**。裸 `{GUID}\name` 走
   `SHCreateItemFromParsingName` / `SHParseDisplayName` / `ShellExecuteExW` 全部
   `0x80070002`；加前缀后图标与启动都成功。前缀拼接统一走
   `shell::ShellNamespacePath()`，它用 `_wcsnicmp` 防重复（重复前缀返回 `E_INVALIDARG`）。
2. **裸 `DesktopAppId` 不能用 `ActivateApplication`**（返回 `0x80070057 E_INVALIDARG`），
   必须 `ShellExecuteExW`。
3. **`IContextMenu::InvokeCommand` 只能按菜单序号调用**（`lpVerb = MAKEINTRESOURCEA(index - 1)`）；
   verb 字符串 + `CMIC_MASK_UNICODE` 返回 `E_INVALIDARG`。

### 图标来源解析顺序（`shell::IconSourceFor`）

`icon_source` 非空 → `Uwp`/`ShellItem` 用 `ShellNamespacePath(aumid)` →
`NamespaceItem` 用 `ShellNamespacePath(namespace_parsing)` → `icon_path` → `target_path`。
`icon_source` 一律是可传给 `SHCreateItemFromParsingName` 的完整 parsing name。

### 图标缓存

- 键：`FNV-1a(UTF-16 source) + size_px`，落盘文件名 `%016llx_%d.png`。
- 目录：`%LOCALAPPDATA%\DockForge\icons`（`core::IconCacheDir()`）。
- 三级查找：内存 → 磁盘 PNG → `IShellItemImageFactory::GetImage` 提取。
- 读盘时校验尺寸必须等于请求尺寸，否则视为 miss 重新提取（防键碰撞与篡改）。
- 落盘失败不阻断绘制；`Get()` 返回 false 只表示这一张取不到。
- 统计口径：`memory_hits`/`disk_hits`/`extractions`/`failures` 互斥计数，
  `average_extract_ms` 只对 `extractions` 求平均。

### 启动结果

`shell::Launch(const AppEntry&)` 返回 `LaunchResult{error, process_id, route}`。
`route` ∈ `"CreateProcess"` / `"ShellExecuteEx"` / `"ActivateApplication"` / `"none"`，
写入日志后即可从一行日志判断走了哪条路径。失败必须返回 `LaunchError` 而非抛异常或崩溃。

## C-09 状态文件编码（T-006 冻结）

所有 `*.md` / `*.json` 状态文件必须是**无 BOM 的合法 UTF-8**。

写入方式（PowerShell 5.1 默认 ANSI，禁止 `Set-Content` / `Add-Content` / `Out-File`）：

```powershell
[System.IO.File]::WriteAllText(, , (New-Object System.Text.UTF8Encoding(False)))
```

校验：`tools/fix_encoding.ps1 -Check`，退出码非零即任务未完成。
修复：`tools/fix_encoding.ps1 -Repair`（按首个非法字节的行边界切分，前半 UTF-8、后半 ANSI 解码后拼接）。

理由见 ADR-0011：混合编码的状态文件会让冷启动协议在第一步就失败，而失败是静默的。

**例外**：`STATE.json` 保留 UTF-8 BOM。PowerShell 5.1 的 `Get-Content` 在无 BOM 时按 ANSI
解码，而 GBK 尾字节可能是 `0x5C`（反斜杠），会静默破坏 JSON 字符串转义。加 BOM 让
PowerShell 5.1 / .NET / Python 三方对同一文件达成一致。Markdown 文件仍无 BOM。

## C-10 显示枚举与 Dock 放置（T-007 冻结）

### 数据结构

`platform::DisplayInfo`：`HMONITOR handle`、`std::wstring device`（`\\.\DISPLAYn`）、
`RECT monitor`、`RECT work`、`UINT effective_dpi`、`UINT raw_dpi`、`bool primary`、
`Width()/Height()/WorkWidth()/WorkHeight()/Scale()`。

`platform::DockPlacement`：`RECT rect`（物理像素，窗口目标矩形）、`int width_px`、
`int height_px`、`UINT dpi`、`float scale`、`bool clamped`。

### 枚举契约

- `EnumerateDisplays()` 的**返回顺序即显示索引**，主屏恒为 `[0]`，其余按 `top`、`left`、
  `device` 字典序，保证同一台机器多次调用顺序一致（`DOCKFORGE_TARGET_DISPLAY` 依赖它）。
- `effective_dpi` 取 `GetDpiForMonitor(MDT_EFFECTIVE_DPI)`，失败回退 `GetDpiForSystem()`；
  `raw_dpi` 单独取 `MDT_RAW_DPI`（本机 192 / 243，两者不等，不能混用）。
- `DisplayForWindow(HWND)` / `DisplayForPoint(POINT)` 返回 `bool`，失败时输出参数保持未定义，
  调用方必须先判返回值。

### 放置契约

- **一律以 `work` 而非 `monitor` 定位**。Dock 压到任务栏底下等于裁切 bug。
  `work` 为空（枚举异常）时回退 `monitor`。
- 超宽/超高时缩小并置 `clamped = true`，禁止越出 `work`。调用方必须 `DF_WARN` 记录。
- 边距单位是 DIP，`margin_px = lround(margin_dip * dpi / 96)`。
- `Bottom`：水平居中，底边贴 `work.bottom - margin`。
  `Left`/`Right`：垂直居中，贴 `work.left + margin` / `work.right - margin`。
- 负原点的副屏（主屏左侧）必须正常工作，禁止假设 `left/top == 0`。

### DPI 与几何必须同源

`Window::Dpi()`（内容 DIP→像素的换算）与 `ComputeDockPlacement`（窗口物理尺寸）必须读同一个
DPI，否则面板会按一个比例布局、装在另一个比例尺寸的窗口里。

- `DOCKFORGE_DPI_OVERRIDE` 设置时优先级最高，且 `WM_DPICHANGED` **不得覆盖它**
  （`main.cpp` 的 `dpi_forced_by_env` 记录该状态）。
- 未设置 override 时，`WM_DPICHANGED` 的 `wParam`（`LOWORD` = x，`HIWORD` = y）是权威 DPI，
  优先于重新枚举的显示器 DPI——枚举在转换瞬间可能滞后。
- `lParam` 的建议矩形**不使用**：它按普通重叠窗口计算，与按工作区放置的 Dock 无关。
- 放置只有一条代码路径（`main.cpp` 的 `apply_placement` lambda），启动、`WM_DPICHANGED`、
  `ShowWindow` 之后都调它。新增定位逻辑必须并入该 lambda，禁止另开分支。

### 验证手段与诚实边界

- `tests/display_test.cpp` 用合成 `DisplayInfo` 覆盖：负原点副屏、96+144 混合双屏、
  超宽/超高 clamp、`work` 全零回退、DPI 往返换算。
- `DOCKFORGE_SIMULATE_DPI=<dpi>` 在进程内 `SendMessageW(WM_DPICHANGED)`，`tools/simulate_dpi.ps1`
  对比前后窗口矩形。**跨进程发送该消息无效**：`PostMessage` 返回
  `ERROR_MESSAGE_SYNC_ONLY`(1159)，`SendMessage` 返回 0 但处理器不执行（`WM_CLOSE` 可跨进程，
  故这是系统消息特有的限制）。
- 单显示器主机上 `GetDpiForWindow` 始终返回真实 DPI。合成消息证明的是**处理器与放置数学**，
  不等于验证了合成器的真实跨屏缩放；不得把两者混为一谈。


## C-11 Dock 渲染契约（T-009 冻结）

### tile 模型

`dock::DockTile { shell::AppEntry app; shell::IconBitmap icon; std::uint64_t icon_key; }`。

- `icon_key` 是**内容身份**（`IconKeyFor(source)`，FNV-1a over UTF-16），不是像素地址也不是 `IconBitmap` 的地址。按指针做键会在 vector 重分配后每帧重传位图；按视图地址会别名到错误内容。
- 同一 `icon_key` 在不同尺寸下视为不同位图，键里必须含尺寸（`IconCache::CacheKey` 已含）。
- 图标提取尺寸固定 `kDockIconPx = 256`，绘制时按槽位尺寸线性降采样。禁止按槽位尺寸提取——槽位尺寸随 DPI 与悬停缩放变化，会击穿缓存。

### 几何来源唯一

`dock::ComputeGeometry(config, tile_count, scale)` 是窗口尺寸、板尺寸、内容宽度的**唯一**计算点。

- `main.cpp` 不得自行累加 `icon_size * n + gap * (n-1)`。T-009 前存在 `LogicalSize` / `ComputeLogicalSize` 与 `apply_placement` 两处独立计算，已删除。
- `ComputeGeometry` 内必须传 `config.dock` 给 `layout::FromConfig`，不是整个 `config`。
- 内容宽 = `BaseContentWidth(count, params) / scale`；板 = 内容 + 2 × `kPlatePaddingDip`；板高 = `icon_size + 2 × kPlatePaddingDip`。
- `kPlatePaddingDip = 10.0f` 是设计常量，**不进 C-01 配置**（用户不需要调板内边距）。

### 绘制顺序

1. 板（`PlateRenderer::Draw`，霜层 / 色调 / 圆角裁剪 / 描边）。
2. 图标（`IRenderBackend::DrawBitmap`）。

顺序不可交换：霜层盖住图标等于没画。图标矩形是**正方形**，边长 = `icon_size × scale`，中心 = `kPlatePaddingDip + layout[i].center / scale`（`layout` 输出的是像素坐标，除以 scale 转 DIP）。

### 缺失图标

`tile.icon.valid() == false` 时画 `StrokeRoundedRect` 淡色圆环占位，**不留空**。

理由：缺图是外观问题，缺启动器是功能问题，两者不能长得一样。留空会让"图标提取失败"看起来像"这个槽位本来就没有应用"。

### 位图 DPI 打标

`ID2D1Bitmap1` 创建时 DPI **固定写 96**，不得写帧 DPI。

目标矩形已是 DIP，后端 `BeginFrame` 设置的变换已带帧 DPI。若位图再打帧 DPI，192 DPI 下会被二次缩放成 1/2 尺寸。

### 验证手段

- `tests/layout_test.cpp` 用 O(n²) 参考实现锁定前缀和等价性。
- `tools/dock_slots.ps1` 逐槽位统计非板色像素覆盖率与包围盒。**禁止用单点采样判断图标存在**：深色中心的图标（7-Zip 黑块）单点会读成板色，被误判为缺失。
- 三档 DPI 必须给出物理窗口尺寸与 `round(DIP × dpi / 96)` 的对照，偏差为 0 才算通过。

### 诚实边界

- 本契约只覆盖**静态图标行**。悬停放大、运行指示、拖拽、缩略图不在其中，各自单独冻结。
- "窗口自动贴合内容宽度"指基础尺寸；悬停时窗口是否扩张属 T-010 决策，不得由本契约推定。

## C-12 Dock 悬停与可点击区域（T-010 冻结）

### 悬停状态

```cpp
struct DockHover { bool active = false; float intensity = 0.0f; float mouse_x_dip = 0.0f; };
inline constexpr float kHoverRampPerSecond = 8.0f;   // 0 -> 1 in 125ms
bool AdvanceHover(DockHover& hover, double delta_ms);
```

- `intensity` 由时间驱动，不由帧数驱动：120Hz 下 10 帧与 60Hz 下 5 帧必须得到相同值（已由测试锁定）。慢帧不得让动画跳跃。
- `AdvanceHover` 返回 `intensity` 是否变化；不超调，`delta_ms <= 0` 时不动。
- 布局按 `magnification_effective = 1 + (magnification - 1) * intensity` 插值。

### 输入合并规则

- `WM_MOUSEMOVE` **只更新状态，不请求重绘**。置 `dock_hover.active = true`、`dock_hover.mouse_x_dip`、`hover_moved = true`。
- tick 处理器消费 `hover_moved`，每帧最多产生一次 `InvalidateRect`。
- 依据：Windows 投递鼠标移动远快于 60Hz。每条消息都重绘时 p99 18.2ms 超预算（实测）。

### 可点击区域规则

- 窗口高度恒为 `icon_size * max(magnification, 1) + 2 * kPlatePaddingDip`，板高恒为 `icon_size + 2 * kPlatePaddingDip`。板上方的空白是放大图标的预留带。
- **`HTTRANSPARENT` 不可用于跨进程穿透**（spike 实测：下层窗口收不到点击）。唯一可用机制是 `SetWindowRgn`。
- `SetWindowRgn` 会裁掉自绘 DComp 内容，因此 region 必须覆盖所有会画出来的像素，否则图标被切断。
- **region 只有两态**：空闲 = 圆角板；悬停 = 圆角板 ∪ 满倍率图标行矩形。不随 ramp 逐帧变化——`SetWindowRgn` 触发 DWM 往返，每帧调用时 p99 17.3ms 超预算（实测）。满倍率矩形是所有中间帧的并集，覆盖等价。
- 悬停行矩形由 `DockPainter::UpdateLayout` 在 `intensity = 1.0` 下算出，不用上一帧结果：用上一帧会漏掉正在放大的图标，把图标在板顶齐平切断（实测 `rows_with_change` 起点等于板顶）。
- 布局 extents 是设备像素，region 也是设备像素，换算只需加原点偏移 `row_origin_dip * scale`。**不得再除 `scale`**。
- `include_row` 判据为 `row.right > row.left && row.top < plate.top`；退化为空时不扩宽。

### 坐标规则

- `WM_MOUSEMOVE` 的 `LPARAM` 是**物理像素**（per-monitor DPI aware 窗口拿到未缩放客户端坐标），必须除以 `window.Scale()` 才能喂给 DIP 布局。
- 所有验证工具在 `DOCKFORGE_DPI_OVERRIDE` 下**必须从窗口高度反推 scale**，不能用 `GetDpiForWindow`——override 只改内容布局，窗口真实 DPI 不变。

### 验证手段

| 断言 | 工具 | 判据 |
|---|---|---|
| 图标真的放大了 | `tools/hover_measure.ps1` | `observed_scale` 与 `peak_scale` 差 < 5% |
| 空闲不吞点击 | `tools/region_probe.ps1`（默认） | `inside_band=0 && inside_plate>=2`，且 `window_from_point` 在带外为 False |
| 悬停不裁图标 | `tools/region_probe.ps1 -Hovered` | `inside_band>=1 && inside_plate>=2` |
| 悬停真的改变了画面 | `tools/image_diff.ps1` | `verdict=DIFFERENT`，`changed_pct > 5` |
| 帧时间达标 | `tools/hover_bench.ps1` | `p99_over_budget_windows=0` |
| 点击穿透机制 | `spikes/t010_hover/region_click.cpp` | `verdict=PASS_THROUGH_OK` |

### 诚实边界

- `region_probe.ps1` 的 `window_from_point` 是 Windows 自己的命中测试结果，比 `PtInRegion` 更接近真实点击；但两者都不是真实点击，端到端点击验证只在 spike 的双窗口场景做过。
- 点击穿透只在 spike 场景验证机制，未在真实第三方应用上验证点击确实落到目标控件。
