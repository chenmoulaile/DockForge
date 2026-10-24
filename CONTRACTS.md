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
| dock.tiles | array\<string\> | []（非托管） | ≤256 项，每项 ≤512 字符 | - |
| dock.hidden | array\<string\> | [] | ≤256 项，每项 ≤512 字符 | - |

**实现约束**
- 未知键：忽略但保留（写回时不得丢失）。
- 越界值：钳制到范围并在日志 WARN，不抛异常。
- 解析失败：整份回退默认值，保留损坏文件为 `config.json.bad`。
- `dock.tiles` / `dock.hidden` 存的是**身份字符串**，不是显示名（见 C-15a）。空数组在序列化时省略，默认配置文件不出现这两个键。
- 列表内的条目校验：非字符串、空串、超长一律丢弃并置 `needs_rewrite`（触发一次自愈回写），不是整份回退默认值——一个坏条目不该抹掉用户的排列。

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

## C-13 窗口注册与点击行为（T-011 冻结）

Dock 的点击语义是「一个槽位三种结局」，判据必须来自实时窗口状态，不能来自启动时的快照。

| 状态 | 判据 | 动作 | `route` |
|---|---|---|---|
| 无窗口 | `FindAppWindow` 未命中 | `shell::Launch` | `launch` |
| 窗口在前台且未最小化 | `GetForegroundWindow()==hwnd && !IsIconic(hwnd)` | `ShowWindowAsync(SW_MINIMIZE)` | `minimize` |
| 其余 | — | `ShowWindowAsync(SW_RESTORE)`（若最小化）+ `SetForegroundWindow` | `restore` / `raise` |

### C-13a 应用窗口筛选（五条全部必要）

`EnumerateAppWindows` 只返回同时满足以下条件的顶层窗口：

1. `IsWindowVisible(hwnd) != FALSE`
2. `GetWindow(hwnd, GW_OWNER) == nullptr`（owned 窗口是对话框与提示，最小化它等于无操作）
3. `(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0`
4. `DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ...) == 0`（cloaked 窗口报 visible，是 shell 用来把窗口停到别的虚拟桌面的手段）
5. `GetWindowTextW` 非空

实测基线（`evidence/T-011/enum.txt`）：空闲桌面 `total_windows=198`、`app_windows=0`；开一个 Notepad 后 `app_windows=1`。`IsWindowVisible` 单独用会把 `ApplicationFrameWindow`、`DummyDWMListenerWindow`、`ForegroundStaging`、`tooltips_class32` 全部放进来。

### C-13b 打包应用的窗口归属

打包应用的可见顶层窗口属于 `ApplicationFrameHost.exe`，不属于应用自己。

- `GetApplicationUserModelId(OpenProcess(frame_pid))` 返回 `0x00003D57`（`ERROR_NOT_FOUND`）——拿不到 AUMID。
- 真实应用进程只能通过 `EnumChildWindows` 找到：frame 窗口下存在一个跨进程子窗口（`class=Windows.UI.Core.CoreWindow`），它的 pid 才是应用 pid。
- 未运行的 frame 窗口（`cloaked=1`）没有跨进程子窗口。
- **因此 `app_pid` 与 `pid` 必须分开存**：`pid` 是顶层窗口所有者（可能是 frame host），`app_pid` 是真正运行应用的进程。

实测（`evidence/T-011/frame.txt`）：Calculator 的 `frame pid=16276`（`ApplicationFrameHost.exe`），跨进程子窗口 `pid=17292`（`CalculatorApp.exe`），后者才报 `Microsoft.WindowsCalculator_8wekyb3d8bbwe!App`。

### C-13c 身份匹配顺序（不可交换）

1. entry 有 `aumid` → **只按 AUMID 匹配**，`window.aumid` 为空即不匹配。
2. entry 无 `aumid` 但有 `target_path` → 按文件身份匹配。
3. 都没有 → 不匹配。

第 1 条不能退化成路径匹配：所有打包应用共享 `ApplicationFrameHost.exe` 这一个路径，退化成路径匹配会让计算器与设置解析到同一个窗口，Dock 会激活错误的应用。

### C-13d 文件身份匹配（不是字符串比较）

`SameExecutable` 先做大小写不敏感字符串比较，不等时再比较 `GetFileInformationByHandle` 的 `dwVolumeSerialNumber` + `nFileIndexHigh` + `nFileIndexLow`。

理由（实测，`evidence/T-011/path_mismatch.txt`）：AppsFolder 里 7-Zip 的条目路径是
`C:\Users\chenk\scoop\apps\7zip\current\7zFM.exe`，
而运行中的进程报
`C:\Users\chenk\scoop\apps\7zip\26.00\7zFM.exe`。
差一个 scoop 版本 junction。字符串比较永远不等，点击每次都会再启动一个副本而不是激活已有的。

### C-13e 前台权限

- `SetForegroundWindow` 从后台进程调用**被拒绝**（返回 0）。
- `AttachThreadInput(our_tid, fg_tid, TRUE)` 之后仍然**被拒绝**。
- 合成 ALT 按下+抬起之后再调 → **成功**。
- 在 `WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST` 窗口的 `WM_LBUTTONDOWN` 里调用 → **成功，无需任何技巧**。

结论：点击处理函数**本身持有前台权限**，`ActivateOrMinimize` 只能从点击路径调用。`alt=1` 出现在正常会话里就是缺陷信号，不是细节——说明点击路径已经失去了前台权限。

### C-13f 异步窗口操作（帧预算）

`ShowWindow(SW_RESTORE|SW_MINIMIZE)` 会**发送并等待**目标应用的消message loop，忙碌的应用会把 Dock 的 UI 线程拖住任意长时间。`ShowWindowAsync` 投递同样请求后立即返回。

实测（`evidence/T-011/click_bench.txt`，20 次点击，预算 16.7ms）：

| 实现 | avg | p50 | p95 | p99 | max | 超预算 |
|---|---|---|---|---|---|---|
| 同步 `ShowWindow` | 41.29 | 14.11 | 91.10 | 494.59 | 494.59 | 10/20 |
| `ShowWindowAsync` | **1.92** | 1.31 | 2.45 | **9.89** | 9.89 | **0/20** |

`SetForegroundWindow` 保持同步：它是快速跨进程请求，不等应用处理，而且是唯一真正需要点击前台权限的调用。

### C-13g 命中测试

`DockPainter::HitTest(x_dip, scale)` 读**当前悬停状态的布局**，不读空闲布局。

- 用空闲布局会让抬起的图标点不到、被它推开的邻居反而可点——指针下的图标正是瞄准的那个，这是最坏的取舍。
- 垂直边界不检查：`SetWindowRgn` 已经把窗口裁成「板 + 放大行」，能进到处理函数的点击一定在可见 Dock 内。
- 点击路径先调 `layout_now()` 再 `HitTest`：移动与下一 tick 之间到达的点击否则会按上一个指针位置判定（16ms 窗口）。

### 验证手段

| 断言 | 工具 | 判据 |
|---|---|---|
| 窗口筛选 | `spikes/t011_click --mode=enum` | 空闲桌面 `app_windows=0`，开一个应用后 `+1` |
| 打包应用归属 | `spikes/t011_click --mode=frame` | frame 窗口存在跨进程子窗口，`real_pid` = 应用 pid |
| 身份匹配 | `spikes/t011_click --mode=aumid` | frame host 的 `aumid_hr=0x00003D57` |
| 点击三分支 | `tools/click_probe.ps1` | `foreground_changed=True`，第二次点击 `previous_foreground_iconic=True` |
| 点击延迟 | `tools/click_bench.ps1` | `p99 < 16.7ms`，`verdict=WITHIN_BUDGET` |
| 空闲开销 | `tools/idle_stats.ps1` | `verdict_cpu=PASS`、`verdict_rss=PASS`、`verdict_handles=PASS` |

### 诚实边界

- 单显示器主机：`restore` 与 `minimize` 都在同一屏幕上验证，跨屏激活未测。
- UWP 分支用「计算器」验证（`DOCKFORGE_PIN_APPS=calculator`），未覆盖所有打包应用；某些应用可能拒绝 `SetForegroundWindow` 而落到 ALT 回退路径。
- `route` 的 `restore`/`raise` 区分依赖动作前的 `IsIconic` 采样，采样与动作之间若被第三方最小化会标错，不影响行为。


## C-14 运行指示器契约（T-012 冻结）

### C-14a 两个时钟，不可合并

| 状态 | 来源 | 实测成本 | 频率 |
|---|---|---|---|
| 应用是否有窗口 | `shell::EnumerateAppWindows()` | 0.48–0.94ms | `RunningTracker::IntervalMs()`，默认 300ms |
| 哪个窗口在前台 | `GetForegroundWindow()` | 亚微秒 | 每帧 |

`TileRunState` 必须是三值（`Idle` / `Running` / `Foreground`）。合并成 `bool running` 会把慢时钟强加给快状态，alt-tab 后高亮滞后最多 300ms。

`RunningTracker::Update(double delta_ms)` 的返回值语义固定为「可见状态是否可能变化」，不是「是否轮询了」。窗口集合未变、前台未变时必须返回 false —— 调用方靠它保持空闲不重绘。

`RunningTracker::Invalidate()` 只影响下一次 `Update` 是否强制重扫，不影响 `Foreground()`。

### C-14b 轮询间隔钳制

`SetIntervalMs` 钳到 **50..5000**。低于 50 会把 0.5ms 的枚举变成每秒 20 次；高于 5000 会让指示器在应用启动后长时间缺失。

`DOCKFORGE_RUNNING_POLL_MS` 是验证钩子（同样钳制到 50..5000），不是用户配置，不得进 C-01。

### C-14c 指示器几何

| 常量 | 值 | 单位 |
|---|---|---|
| `kIndicatorDotDip` | 4.0 | DIP 直径 |
| `kIndicatorOpacityForeground` | 0.95 | — |
| `kIndicatorOpacityRunning` | 0.45 | — |
| 后台圆点宽度系数 | 0.85 | 倍数 |

圆点垂直中心 = `geometry.baseline_dip + kPlatePaddingDip * 0.5`，即板的底部内边距中央。

**约束**：`kIndicatorDotDip < kPlatePaddingDip` 必须由测试断言，不得只写在注释里。板的底边就是窗口底边，圆点超出内边距会同时落在板和窗口 region 之外，被 `SetWindowRgn` 裁掉。

**禁止新增指示器带**。板的 `plate_height_dip` 与窗口 `window_height_dip` 不得因指示器而改变：改高会让屏幕上每个 Dock 位移，并让所有从窗口高度反推 scale 的验证工具失效。

### C-14d 状态写入位置

`run_state` 写在 `dock::DockTile` 上，由 `RunningTracker::ApplyToTiles` 整体覆写，不得使用并行数组。tile 列表重建时并行数组会失步，而失步的表现是圆点指向错误的图标。

`ApplyToTiles` 必须覆盖每个 tile（包括把陈旧的 `Foreground` 改回 `Idle`），并接受空列表。

### C-14e 前台归属判定

判定顺序（不可交换）：
1. 命中的窗口 `hwnd` 等于 `GetForegroundWindow()`
2. 否则从 `GetForegroundWindow()` 起 `GetAncestor(GA_ROOTOWNER)` 向上最多 8 层，命中即 `Foreground`
3. 否则取前台窗口的 pid，等于命中的 `app_pid` 或 `pid` 即 `Foreground`
4. 其余为 `Running`

第 3 步不可省：`GA_ROOTOWNER` 不保证落到应用顶层窗口，对话框/弹出窗口会漏判。

### C-14f 身份匹配沿用 C-13c

`StateFor` 必须调 `shell::FindAppWindow`，不得另写匹配逻辑。这意味着指示器与点击行为对「哪个窗口属于这个应用」的判断永远一致 —— 否则会出现「点了能激活但没圆点」或反向的自相矛盾。

已知后果：文档类条目（条目指向 `.chm`、实际运行 `hh.exe`）不显示圆点。这是真实归属，不是缺陷。

### C-14g 验证手段

| 手段 | 工具 | 判据 |
|---|---|---|
| 圆点存在与亮度 | `tools/indicator_probe.ps1` | `max_delta` 与板色距离；前台 > 后台 × 1.8 |
| 四态覆盖 | `tools/t012_evidence.ps1` | `verdict=ALL_PASS` |
| 退出延迟 | 同上 | `exit_detected=True` 且 `elapsed_ms <= 1000` |
| 轮询成本 | 每秒 stats 行的 `run polls= avg= max=` | `avg < 1ms` |
| 帧预算归因 | `DOCKFORGE_RUNNING_POLL_MS` 交替 A/B | 差值需超过同配置运行间波动才算归因 |

`indicator_probe.ps1` 的两条硬约束：
- 行原点必须由窗口宽度与内容宽度算（`(window_dip - content_dip) / 2`），不得用 padding。
- 板色必须从被采样行自身取众数（`-Plate auto`），不得硬编码。solid 材质仍混合 8% 背景，板色随背景从 `(13,15,18)` 变到 `(33,35,38)`。

### C-14h 诚实边界

- 单显示器主机，跨屏时指示器跟随未验证。
- 同一进程多窗口时只高亮第一个命中窗口；逐窗口指示依赖 T-015 的窗口列表。
- 帧预算判 WATCH 而非 PASS，理由见 `evidence/T-012/ab_tracker_cost.md`。

## C-15 Dock 拖拽与排列（T-013 冻结）

### C-15a 身份字符串（不是显示名）

`dock.tiles` / `dock.hidden` 里存的是 `dock::IdentityOf(AppEntry)` 的输出，取值优先级：

1. `aumid`（打包应用 / 裸桌面 id）
2. `namespace_parsing`（`{GUID}\name` 形态）
3. `target_path`（完整路径，**不是文件名**）
4. `display_name`（最后兜底）

全部经 `LowerAscii`（只转 A-Z，不做 locale 折叠）后存储。选择路径而不是文件名，是因为同一目录下不同子目录的同名 exe 是常见布局（`7zip\current\7zFM.exe` vs `7zip\26.00\7zFM.exe`），文件名会撞键。

**身份必须在扫描后重新计算，不可从旧配置反查**：配置里存的是上一次的身份，条目可能已被卸载或换了安装路径。

### C-15b 非托管 / 托管两种模式

| `dock.tiles` | 行为 |
|---|---|
| 空 | **非托管**：按 AppsFolder 扫描顺序自动填充前 `kDockTileLimit`（12）项，跳过 `dock.hidden` |
| 非空 | **托管**：只显示列表里点名的条目，**不自动补位** |

托管模式不自动补位是刻意的：删掉一个 tile 后立刻由扫描序下一个顶上，用户会读成"删除没生效"。代价是列表比可用条目长时 Dock 会变短，这是可接受的——变短是用户能看见并理解的结果。

`dock.hidden` 在两种模式下都生效，且**优先于** `dock.tiles`（同一身份同时出现在两处 = 隐藏）。

### C-15c 重排语义

`MoveTile(tiles, from, to)` 是 **erase + insert**，不是 swap。把第 1 个拖到第 4 个，中间三个要左移一位；swap 会让另外两个 tile 出现在用户没预期的位置。

插入槽由 `InsertionIndexFor(pointer_x, centers, source_index)` 决定：

- 数严格小于指针的 centre 个数，再减去源槽自身（源槽在指针左侧时才减）。
- 比较用 `>=`：指针恰好落在某槽中心 = 正压在那个图标上，拖到图标上就该占它的位置。用 `>` 会让目标永远差一槽（合成指针从不停在中心，所以只对真人才暴露）。
- 结果 `clamp(0, count-1)`。
- **centres 必须是静止栅格**（`DockPainter::IdleCentersDip`），不是放大后的中心。放大后的中心是鼠标位置的函数，喂回去会形成反馈环：向右移 → 放大右侧 → 中心更靠右 → 改变"已越过哪个槽"。实测该环让 `0 -> 4` 的拖拽报成 `0 -> 3`。

### C-15d 手势状态机

`DragPhase{Idle, Pending, Dragging, Ejecting}`，阈值 `kDragThresholdDip = 4.0f`。

- 阈值从**按下点**测量，不是上一帧。从上一帧测量会让慢速拖拽每帧都不足 4 DIP，永远不启动。
- `Pending` 期间释放 = `Click`（走正常激活/最小化），不是 `Reorder`。
- `Dragging` 期间 `insert_index == source_index` 时释放也判 `Click`：拖出去又拖回原位不该重写配置。

### C-15e 移除需要长按确认

`kDragEjectDip = 48.0f`，`kEjectConfirmMs = 550.0`。

- 只判**上、左、右**，不判下方。Dock 贴工作区底边，向下滑落是最容易的误操作，判成移除会在误点时删 tile。
- 窗口尺寸 ≤0 时 `IsEjectPosition` 恒 false（启动期几何未定）。
- 进入 eject 区后**不立即移除**：`DragState::eject_hold_ms` 由 `DragController::Tick(delta_ms)` 累加，达到 550ms 才在 `End()` 报 `Remove`，否则报 `Cancel`。
- `Tick` 必须由帧循环调用，**不能挂在 `Move` 上**：用户被要求"按住"时最自然的动作就是停下不动，而 `Move` 只在移动时触发。
- 进出 eject 区时 `eject_hold_ms` / `eject_progress` 归零。跨一次滑出滑入累计会让一次抖动删掉 tile，这正是确认要防的事。
- `Tick` 在进度满额后返回 false，停止请求重绘——否则一直按住会空转帧循环。
- 唯一的反馈是 ghost 随 `eject_progress` 淡出（`kDragGhostOpacity * (1 - 0.65 * progress)`）。Dock 是 `WS_EX_NOACTIVATE`，弹确认框会抢焦点，比误删更糟。

### C-15f 拖拽期间的 region 与重绘

- 按下即调 `apply_region()` 把 region 扩到**整个窗口**。`SetWindowRgn` 会裁剪合成内容（C-12），不扩则 ghost 一离开板顶就被切掉（实测被切在 `y=317`，板顶）。
- 拖拽期间 `apply_region` 不再走 hover 两态逻辑；释放后恢复。
- `WM_MOUSEMOVE` 只更新 `DragController` 状态并置 `drag_moved`，由 tick 合并成每帧最多一次重绘。
- `WM_MOUSEMOVE` 在拖拽态**提前 return**，不再更新 `dock_hover.mouse_x_dip`：拖拽期间行放大围绕按下点保持不动，行不随指针横移而重排。这不是遗漏，是让插入线可读——行跟着指针跑的话，插入线永远在指针正下方，看不出要插到哪。

### C-15g 持久化时机

`CommitOrder(config, tiles, removed_identity)` 在**手势结束时立即** `core::SaveConfig`，不等退出。手势结束后崩溃会丢掉用户刚做的排列，而排列无法从扫描结果重建。

`dock.hidden` 只增不剪：临时缺失的应用（U 盘、未挂载的网络盘）会丢掉删除记录。重复移除同一身份不重复追加。

### C-15h 测试隔离

`core::ConfigPath()` 支持 `DOCKFORGE_CONFIG_PATH` 覆盖。`tests/main_test.cpp` 的 `ConfigIsolation` 在静态初始化期把它指向 `%TEMP%\dockforge-tests-<pid>\config.json`。

**任何调用 `SaveConfig` 的测试都必须依赖这个隔离**：`CommitOrder` 的用例曾经把 `tiles:["alpha.app"]` 写进开发机真实配置，下次启动 Dock 只剩 1 个 tile。测试跑完必须验证真实配置未被触碰。

### C-15i 验证手段

| 断言 | 手段 | 判据 |
|---|---|---|
| 重排生效且索引正确 | `tools/drag_probe.ps1` 读 app 日志 | `reordered tile[a] -> [b]`，a/b 与注入的起止槽一致 |
| 移除生效 | 同上 + 配置回读 | `removed tile[n]` + `dock.hidden` 含该身份 |
| 提前释放不删除 | `-EjectAbort` | 无 `removed` 行，配置字节不变 |
| 捕获真的建立 | app 自报 `drag press ... capture=1 match=1` | 不读探针进程的 `GetCapture()` |
| ghost + 插入线画出 | `tools/drag_visual.ps1` 差分 | 拖拽帧 vs 悬停控制帧：ghost 变化像素聚集在指针周围，插入线是 ≤10px 宽的亮列 |
| 拖拽帧率 | `tools/drag_bench.ps1` | `fps_min >= 55` 且 p99 窗口超预算数 = 0 |

### C-15j 诚实边界

1. **`GetCapture()` 不能跨进程断言**。它返回调用线程的捕获窗口，探针进程读永远是 0。第一版探针据此报 `capture_taken=False`，差点被当成"捕获没建立"去改设计。断言必须来自 app 自己打的日志。
2. **拖拽视觉验证需要控制帧**，且控制帧必须停在**按下点**而不是释放点。拖拽期间行放大围绕按下点保持（C-15f），停在释放点的控制帧会放大另一个图标，差分结果全是图标横移，证明不了 ghost。
3. **单显示器主机无法验证跨屏拖拽**。拖到副屏需要两个物理屏；本机只有一块 2880x1920，跨屏行为未测。
4. **`drag_bench.ps1` 必须先等 `frames=1000`**。`FrameClock` 是 1000 样本滚动窗，启动期的大样本（首帧 309ms）要在 1000 个新样本后才滚出；不等就测，会把启动成本算成拖拽成本（实测报 p99=91.36ms / OVER_BUDGET，而稳态每窗 16.56–16.66ms）。

## C-16 外部拖入（OLE drop，T-014 冻结）

Dock 接受从资源管理器、开始菜单、桌面拖来的对象。这一节冻结"什么算可拖入"、"落点怎么算"、"OLE 有哪些硬约束"，以及用什么证据判定它真的工作。

### C-16a 五类载荷

`shell::DropKind` 由路径 + 文件属性决定，**不看能否取到图标**：`IShellItemImageFactory` 对 `.txt`、目录、任何路径都返回 `S_OK` 和一张图（实测 `.txt` 冷 92.17ms / 热 0.31ms，目录冷 56.85ms / 热 0.31ms），所以"能取到图标"不能作为可接受性判据。

| DropKind | 判据 | 是否可拖入 |
|---|---|---|
| `Shortcut` | 属性非目录且扩展名 `.lnk` | 是 |
| `Executable` | 属性非目录且扩展名 `.exe`/`.cmd`/`.bat`/`.com` | 是 |
| `Folder` | 属性含 `FILE_ATTRIBUTE_DIRECTORY` | 是 |
| `Unsupported` | 其余已存在的文件（文档、图片、压缩包……） | 否 |
| `None` | 空路径，或 `GetFileAttributesW` 返回 `INVALID_FILE_ATTRIBUTES` | 否 |

扩展名比较大小写不敏感。**文档类不接受**：pin 一个 `.txt` 得到的 tile 点下去只会启动编辑器，dock 会显得在随机开程序。

`IsDockable(DropKind)` 是唯一判据，`DropKind::None` 与 `Unsupported` 为假。

### C-16b 混合载荷整体拒绝

`ClassifyDroppedPaths` 对多文件载荷**全部可接受才返回可接受**，否则整体 `Unsupported`。静默接受一半会让用户无法知道哪部分生效，而 dock 的 tile 顺序是用户唯一能核对的凭据。

### C-16c `OleInitialize` 是硬前置

`RegisterDragDrop` 在 `CoInitializeEx(COINIT_APARTMENTTHREADED)` 之后返回 **`0x8007000E E_OUTOFMEMORY`**（加不加 `COINIT_DISABLE_OLE1DDE` 都一样），在 `OleInitialize(nullptr)` 之后返回 `S_OK`。同一进程、同一窗口、只换初始化方式，实测三组（`evidence/T-014/run_reg_oleinit.txt`、`run_reg_coinitapt.txt`、`run_coinit_target.txt`）。

因此 `main.cpp` **不得调用 `CoInitializeEx`**，OLE 生命周期由 `dock::OleSession` 独占：引用计数 RAII，首次构造 `OleInitialize`，末次析构 `OleUninitialize`。`E_OUTOFMEMORY` 是误导性错误码，日志里必须原样记录 HRESULT，否则现场只会看到"拖不进去"。

### C-16d 拖入时行回落 + 静止栅格

外部拖入期间：

- `dock_hover.active` 强制为 `false`，行通过正常 ramp 回落。原因：放大后的行会把图标移到指针旁边，而落点槽位是按静止栅格算的，两者不一致时 caret 指的图标不是松手后真的落进去的位置。
- 落点槽位与 caret 都用 **`IdleCentersDip(geometry, scale)`**：静止栅格是解析式（`origin + (i*pitch + half)/scale`），不是放大布局的输出。用放大布局的 `IconCentersDip` 会产生反馈环——指针右移放大右侧图标，中心更右，反过来改变"已越过哪个槽"（T-013 实测错位 1 槽）。
- `InsertIndexFor` 用 `x_dip >= centre` 计数，`clamp(passed, 0, size)`，`size` 表示追加到末尾。`>=` 而不是 `>`：指针正好压在图标中心时算压在那个图标上。

**scale 必须与几何同源**。`DockDropTarget::SetScale(window.Scale())`，禁止在 `UpdatePointer` 里读 `GetDpiForWindow`：`DOCKFORGE_DPI_OVERRIDE` 下窗口保持真实 DPI 而行按 override 排布，两者比值为 2 时每个 x 都被减半（实测 96 DPI override 报 `scale=2.000 x=188.5dip`，实际行原点 69.4 DIP、槽距 56 DIP，落点算成 tile[3] 而真实是 tile[6]）。这条与 C-10「几何与内容读同一 DPI」是同一条规则的第二次发作。

### C-16e caret：位置、颜色、形状

落点提示是**一条竖线**，画在静止栅格的目标槽位上：

| 属性 | 值 |
|---|---|
| 宽度 | `kDropCaretWidthDip = 4.0f` DIP（圆角矩形，圆角 = 半宽） |
| 高度 | `icon_size * kDropLineHeightFactor`（0.62），与内部重排的插入线同高 |
| 垂直位置 | 居中于图标行（`baseline_dip - icon_size * 0.5f`） |
| 可接受色 | `kDropLineAcceptColor{0.92f, 0.94f, 0.98f, 0.90f}`（近白） |
| 不可接受色 | `kDropLineRejectColor{0.95f, 0.42f, 0.38f, 0.90f}`（暖红） |
| 绘制时机 | `input.drop->active` 为真时，**在 `if (!dragging) return true;` 之前**（外部拖入与内部重排互斥，但不能因为内部重排分支提前返回就不画） |

**不可接受时也要画**。系统光标上的"禁止"徽标由源进程绘制，一旦指针进入一个报 `DROPEFFECT_NONE` 的窗口就消失——那会让 dock 在松手前一刻看起来是接受的。红色 caret 是 dock 自己能保证的反馈。

caret 位置解析式计算（`row_origin_dip + slot * pitch_dip`），**不建 centre vector**：`DragOver` 每秒几十次，每次分配会进热路径。

### C-16f region 只在两态翻转时更新

`DragEnter` 时把 region 扩到整窗（`drag.Active() || drop_pointer.active || full_region_probe` → `row = {0,0,plate.right,plate.bottom}`），`DragLeave` / `Drop` 时恢复。理由：

- region 外的 `DragEnter`/`Drop` **完全不投递**。实测边界严格等于 region 顶边：窗口 800x200、region 从 client y=80 起，drop_y=39% 收不到、40% 收到（`evidence/T-014/run_edge_39.txt` / `run_edge_40.txt`）。
- 进入后不扩 region，指针上移到板上方会收到 `DragLeave` 且 `Drop` 永不触发；源进程的 `DoDragDrop` 卡在 `QueryContinueDrag -> DROP` 之后不返回（`run_shrink_noexpand.txt`，进程须强杀）。
- **不能每次 `DragOver` 都 `SetWindowRgn`**：那是 T-010 测过的每帧 region 反模式（p99 17.3ms）。只在 `region_drop` 由 false 变 true、由 true 变 false 时调用。

### C-16g 回调只记录状态，重绘由 tick 合并

OLE 回调运行在源进程的模态拖拽循环里，是**对我们消息循环的同步打断**。回调里：

- 允许：写 `drop_pointer`、翻转 `region_drop`、调 `apply_region()`（仅翻转时）、置 `drop_moved = true`。
- 禁止：`InvalidateRect`、枚举窗口、读配置、任何可能阻塞的调用。

`drop_moved` 由 tick 消费，每帧最多一次重绘。实测：每个 `DragOver` 都 `InvalidateRect` 时 `p99_mean=17.284ms / p99_max=18.490ms / 19-20 窗超预算`；改成 tick 合并后 `p99_mean=16.512ms / p99_max=16.540ms / 0 窗超`（`evidence/T-014/drop_bench.txt`）。与 T-010 的 `hover_moved`、T-013 的 `drag_moved` 是同一条规则的第三次应用。

### C-16h 落盘：查重、逆序、立即持久化

- **查重按身份字符串**（`dock::IdentityOf`，与 C-15a 同一个函数）。已存在时只把 tile 移到目标槽位，不新增。
- **逆序插入**（`paths.rbegin()..rend()`）：每次 insert 会把后面的右移，正序插入会让第一个路径落到最后一个位置。
- **立即 `CommitOrder`**，不等到退出：drop 是用户可见的一次性动作，崩溃丢排列比多一次磁盘写更糟。
- 新增 tile 后必须 `apply_placement()`：窗口宽度依赖 tile 数（12 tile 1510px、13 tile 1622px、14 tile 1733px @ 192 DPI），不重新放置会裁掉新图标。

### C-16i 验证手段

| 断言 | 手段 | 判据 |
|---|---|---|
| 可接受性分类 | 单元测试 `tests/dock_drop_test.cpp` | 五类 kind + 混合载荷 + 属性优先（叫 `notes.txt` 的目录算 Folder） |
| OLE 初始化 | spike 三组对照 | `OleInitialize` → `S_OK`；`CoInitializeEx` → `0x8007000E` |
| region 边界 | spike 逐百分比 drop | 39% 无 `DragEnter`，40% 有 |
| region 扩张必要性 | spike 两段路径 | 扩张：`Drop` 到达；不扩张：`DragLeave` 且源进程挂住 |
| 端到端四类载荷 | `tools/t014_evidence.ps1` | shortcut 新增、shortcut_again 不重复、document 拒绝、folder 新增 |
| caret 存在与槽位 | `tools/drop_caret.ps1` | 带内变化覆盖率 ≥60% 的**单一 ≤12px 宽 run**，中心与 `ExpectedX` 差 ≤14px |
| caret 颜色 | 同上 `-ExpectedTint` | 峰值像素 R 比 B 高 40 以上判 reject，否则 accept |
| 拖入帧预算 | 日志 `frames=` 行 | p99 超预算窗口数 = 0，`fps_min ≥ 55` |
| 空闲成本 | `tools/idle_stats.ps1` | CPU <0.5%、RSS <120MB、句柄无增长 |

`tools/drop_caret.ps1` 的三条实现约束，都是踩过才写下来的：

1. **按列统计而不是整帧 bbox**。caret 4 DIP 宽、约 30 DIP 高，整帧差分里只占 0.16%，bbox 被行回落或图标高亮主导（96 DPI 实测 bbox `0,57..754,105` 跨整行）。
2. **宽度是唯一区分器**。图标边缘 48 DIP、行回落数百像素，只有 caret 是几像素宽。
3. **颜色取 run 内变化最强的像素，不取首个合格列**。caret 抗锯齿，边缘列会向板色混合：192 DPI 下首个合格列读成 `(46,28,29)`（近黑），峰值是 `(219,98,89)`。用边缘列会把 reject 判成 accept——正是这个检查要防的错误。

### C-16j 诚实边界

1. **截图快门必须由 app 日志驱动**。固定 `Start-Sleep` 实测在拖入前 ~200ms 开快门，得到 `changed_pixels=0 IDENTICAL`，会被读成"caret 没画"。`tools/drop_visual.ps1` 轮询日志出现 `drop enter` 后再等 `DwellMs/2`。
2. **blur 档无法截图**。该档 `WDA_EXCLUDEFROMCAPTURE` 让 `CopyFromScreen` 返回桌面背后内容，dock 区域是黑的。视觉证据一律 `DOCKFORGE_FORCE_MATERIAL=solid`；这是快门的限制，不是 caret 的限制。
3. **多文件拖入未做视觉证据**。`ClassifyDroppedPaths` 的整体拒绝逻辑有单元测试，但没有真实多选拖拽的端到端截图。
4. **单显示器主机**。跨屏拖入未测。

---

## C-17 窗口缩略图预览（T-015 冻结）

C-11 只覆盖静态图标行。悬停放大（C-12）、运行指示（C-14）、拖拽（C-15）、外部拖入（C-16）各自单独冻结，缩略图预览同样单独冻结，本节即其契约。

### C-17a 触发条件

悬停同一 tile 连续 **400ms**（`dock::kPreviewDwellMs`）才弹出预览。400ms 是设计常量，不进配置表——与 `kHoverRampPerSecond` 同例：它是交互节奏而非用户偏好。

下列任一发生即取消并重置计时：指针离开 dock、切换到另一 tile、按下左键、拖拽开始、外部 OLE 拖入、DPI 变化。预览只对"静止指向"响应，不对手势响应。

### C-17b 预览窗口是独立顶层窗口，不是 dock 的一部分

| 属性 | 取值 | 理由 |
| --- | --- | --- |
| 所属 | `platform::PreviewWindow`，自有窗口类 `DockForgePreviewWindow` | 不能复用 `platform::Window`：同类 WndProc 在 `WM_DESTROY` 时 `PostQuitMessage`，会结束 dock 消息循环；其消息 hook 属主 dock |
| 扩展样式 | `WS_EX_TOOLWINDOW \| WS_EX_NOACTIVATE`，`WS_POPUP` | 不进 Alt-Tab、不抢焦点 |
| **禁止** `WS_EX_NOREDIRECTIONBITMAP` | 不加 | DWM 把缩略图合成在 dest 的 redirection surface 之上，GDI 底必须保持可见（spike 实测结论 1） |
| 不用 `CompositionSurface` | 是 | dock 的 surface 是 DComp visual，会吞掉 DWM 合成层；预览窗用普通 GDI 画底 |
| 生命周期 | 隐藏而非销毁，`Place()` 才显示 | 窗口创建成本不重复付；但 DWM 注册必须在隐藏前释放 |
| 区域 | 不设 `SetWindowRgn` | dock 的 region 会裁掉弹窗且高度固定 |

### C-17c 槽位几何（常量，来自 spike 实测）

| 常量 | 值 | 来源 |
| --- | --- | --- |
| `kMaxThumbnails` | 8 | DoD 上限，也是 spike 实测稳定数 |
| `kSlotWidthDip` | 110 | 8/8 渲染的最大槽宽 |
| `kSlotHeightDip` | 70 | 同上 |
| `kSlotGapDip` | 8 | 同上 |
| `kSlotPaddingDip` | 12 | 槽与弹窗边的间距 |

**渲染数量经验上限**（192 DPI，同目标窗注册 N 个后按槽实测）：110x70 / 120x90 / 130x70 / 130x130 / 140x70 DIP 均 8/8；150x70 / 160x70 / 170x70 报 7；180x70 报 6；200x130 只 4/5/6。已排除注册节奏、重注册、脏区、源重绘、TOPMOST、布局方向、注册顺序、槽高。未定因（非总像素预算，非窗口宽度）。**工程结论即取 110x70 + gap 8，不追因**——spike 三次全 8/8 稳定。

弹窗尺寸（DIP）：`宽 = 2*padding + n*(slot_w+gap) - gap`，`高 = 2*padding + slot_h`。水平中心对准悬停图标的静止栅格中心，垂直置于 dock 上方 8px。

### C-17d 注册必须分帧，不可同步

冷 `DwmRegisterThumbnail` 实测 **219–609ms/槽**（8 槽共 594ms）。同步注册会在一帧内吹穿 16.7ms 预算——而那一帧正是用户索要预览的一帧。

规则：**弹窗先显示（空槽），注册按每 tick 1 个的速率补齐**（`kRegistrationsPerTick = 1`）。`ThumbnailHost::SetSources` 返回立即完成数，余下进 `pending_`，由 `PumpPending()` 在后续 tick 消费。预览因此先出框后出图，最多约 8 帧填满。

注册失败的槽静默跳过并记 `DF_WARN`，不影响其余槽；来源窗口已死（`!IsWindow`）直接跳过。

### C-17e DWM 矩形单位是 dest 客户端物理像素

`DWM_THUMBNAIL_PROPERTIES::rcDestination` 用**物理像素**。传 DIP 在 200% 屏上整体半偏移（spike 注释实测）。槽矩形由 `kSlotPaddingDip + i*(slot_w+gap)` 乘 `scale` 得出，`scale` 与几何同源（`window.Scale()`），禁 `GetDpiForWindow`——与 C-16d 同一条规则。

`fSourceClientAreaOnly = FALSE`：窗口框属于缩略图内容，裁掉会让预览读成"丢了标题栏"。

### C-17f 最小化不黑屏

`ShowWindow(SW_MINIMIZE)` 后泵 700ms 采样仍是末帧（`got=(134,202,154) best_delta=0`）——**DWM 保留末帧，应用层不缓存**。但进程退出后缩略图空白，故来源窗口销毁时应用层必须移除槽位（`SetSources` 重算时自然处理）。

### C-17g UWP 经 ApplicationFrameHost 枚举正确

打包应用的可见顶层窗口由 `ApplicationFrameHost.exe` 托管，真实 `app_pid` 只能经 `EnumChildWindows` 跨进程拿到（C-13b）。`window_registry.cpp` 已在 `AppWindow.app_pid` 记录该真实 pid，而 `AppWindow.hwnd` 是 frame host 的 HWND。

**缩略图源 HWND 用 `AppWindow.hwnd`**（frame host）。DWM 对 frame host 与真实窗都返回 S_OK，且缩略 frame host 才显示应用真实内容。日志同时记录 frame hwnd / app_pid / 选中源，作为 DoD 第 4 条的证明。

### C-17h 窗口列表归属，且不得另写匹配逻辑

多窗口列表 = `shell::WindowsForEntry`：对每个 tile，所有匹配该 `AppEntry` 的顶层窗口，按 z 序（MRU 优先）。

它**复用** `dock::IdentityOf`（C-15a 的身份字段顺序）与 `window_registry` 的窗口侧身份（AUMID 优先，否则 exe 路径，同样 Lowercase），不实现第二套匹配——C-14f 已冻结该禁令。窗口列表来源是 `RunningTracker::Windows()`（距上次轮询 ≤300ms、且已付费），预览帧内不再 `EnumWindows`。

C-14h 承诺的"逐窗口指示依赖 T-015 的窗口列表"即此函数，但**逐窗口指示本身不在 T-015 范围内**——本任务只交付列表与预览。

### C-17i 句柄无泄漏

每个 `DwmRegisterThumbnail` 必须与 `DwmUnregisterThumbnail` 配对，且在 dest 窗口销毁**之前**调用。`ThumbnailHost::ReleaseAll()` 是唯一注销入口，`HidePreview()` 与进程收尾都走它；`preview.Destroy()` 在其后。指向已销毁 HWND 的注册是桌面合成器内的悬空引用，只在关机挂起时才暴露。

### C-17j 验证手段

| 项 | 手段 | 判据 |
| --- | --- | --- |
| 悬停 400ms | `tools/preview_probe.ps1` 读日志 `preview shown tile=` | 悬停 <400ms 无该行；≥400ms 有 |
| 多窗口横向 ≤8 | 同一 app 开 N 个窗口后悬停 | 日志 `windows=N`，视觉 N 槽，N>8 只出 8 |
| 最小化不黑 | 最小化源窗后悬停 | 槽内仍是末帧（非纯黑板） |
| UWP 枚举 | Calculator 悬停 | 日志 `frame hwnd`/`app_pid` 不同且内容正确 |
| 句柄无泄漏 | `tools/preview_handles.ps1`，40 轮显隐 | GDIOBJECTS/USEROBJECTS 前后一致 |
| 帧预算 | `tools/hover_bench.ps1`，A/B 交替（预览开/关） | p99 ≤16.7ms，且 p99 差 <0.018ms（同配置运行间噪声底，T-012 实测） |

`tools/hover_bench.ps1` 的实现约束（沿用 T-014/T-012 的三条教训）：

1. **快门由日志驱动，不用固定 sleep**。固定 sleep 实测在事件前 ~200ms 开快门，得到 `changed_pixels=0`，会被读成"没弹预览"。
2. **A/B 交替，单次对比不成立**。同配置运行间波动 0.018ms 与待测差值同量级（T-012 实测），必须 `DOCKFORGE_PREVIEW=1` 与 `=0` 交替跑，差值低于噪声底时判"无信号"而不是"更快"。
3. **必须等 `frames=1000`**。`FrameClock` 是 1000 样本滚动窗，启动期首帧 309ms 会污染前 16 秒的 p99（C-15i）。

视觉证据一律 `DOCKFORGE_FORCE_MATERIAL=solid`（C-16j 第 2 条同因：blur 档 `WDA_EXCLUDEFROMCAPTURE` 使 `CopyFromScreen` 拍不到 dock）。

### C-17k 开关

`DOCKFORGE_PREVIEW=0` 关闭预览。默认开启，关闭时零成本（tick 内一次布尔短路，不建窗、不注册）。

### C-17l 诚实边界

1. **单显示器主机**。跨屏 dock 上的预览定位未测；`dock_rect` 已用虚拟屏坐标，但未验证副屏负原点场景。
2. **DWM 渲染上限未定因**。110x70 稳定 8/8，但为何更大槽位掉槽未查清；契约选择绕开而非解释。
3. **多窗口 >8 时的取舍未定**。当前截断到 8，未实现"哪 8 个"的策略（MRU 是当前顺带结果，非设计决策）。
4. **预览内点击**。当前点击关闭预览，未实现"点击预览激活对应窗口"。
5. **逐窗口指示未实现**。C-14h 依赖的列表已交付，指示本身仍是单圆点。

## C-18 Launchpad 应用索引与文件夹（P1 冻结）

**索引来源**

Launchpad 的应用列表由三个来源合并，优先级从上到下：

| 来源 | 实现 | 说明 |
|---|---|---|
| Shell AppsFolder | 复用 `shell::ScanAppsFolder()`（C-08） | 与 Dock 同一入口。已含开始菜单全部 `.lnk`、UWP 打包项、注册了 AUMID 的 Win32 项 |
| 自定义目录 | `launchpad.extra_paths` 每项递归 `shell::FindShortcuts(root, 4)` | 复用 C-08 的 `.lnk` 解析，不另写 |
| 用户手动添加 | `launchpad.folders[].items` 里的身份字符串 | 只在文件夹展开时解析，不进主网格 |

**禁止**再写一套应用枚举。`src/shell/app_resolver` 与 `src/shell/start_menu` 是唯一入口；Launchpad 只做聚合、去重、排序。

**去重规则**

身份字符串按 C-15a 的优先级生成（`aumid` → `namespace_parsing` → `target_path` → `display_name`，全 LowerAscii）。同一身份只保留一条，保留先出现的（AppsFolder 优先于自定义目录）。

**排序**

默认按显示名做 `CompareStringEx` 的 `SORT_DEFAULT|NORM_LINGUISTIC_CASING` 排序，不做 ASCII 排序——中文名与大小写混排必须符合用户预期。文件夹排在应用之前，文件夹内部按同样规则排序。

**文件监视**

`ReadDirectoryChangesW` 监视开始菜单两个 Programs 目录与 `launchpad.extra_paths`，防抖 500ms。监视失败只 WARN，不阻塞 Launchpad 打开（退化到"下次打开时重扫"）。

**launchpad 配置块（C-01 扩展，v1 向后兼容）**

| 键 | 类型 | 默认 | 范围 | themeable |
|---|---|---|---|---|
| launchpad.extra_paths | array\<string\> | [] | ≤16 项，每项 ≤512 字符 | 否 |
| launchpad.columns | int | 0（自适应） | 0..12，0 = 按屏宽算 | 是 |
| launchpad.icon_size | int | 96 | 48..192 | 是 |
| launchpad.folders | array\<object\> | [] | ≤64 组 | 否 |
| launchpad.page_size | int | 35 | 10..70 | 否 |

`launchpad.folders` 元素：

```
{ "id": "uuid-v4-lowercase", "name": "string(≤64)", "items": ["identity", ...] }
```

约束：`id` 必须唯一且非空，重复则保留首条并 WARN；`items` 空数组的文件夹在加载时丢弃（空文件夹无意义）；`items` 内的身份在解析失败时跳过并 WARN，不使整组失效。

**分页**

单页最多 `page_size`（默认 35 = 5×7）。列数 = `launchpad.columns` 非 0 时取其值，否则 `clamp(floor(可用宽度 / (icon_size + gap)), 3, 12)`。行数 = `ceil(page_size / columns)`。页码指示器只在页数 > 1 时出现。

## C-19 Stage Manager 分组（P2 冻结）

**模型**

```
stage.groups: [
  { "id": "uuid-v4-lowercase", "name": "string(≤64)", "hwnds": ["0x1a2b3c", ...], "icon": "path|null" }
]
```

- `hwnds` 存**十六进制小写、无前导零补齐**的字符串（如 `"0x1a2b3c"`）。存字符串而非数字，因为 JSON 数字是 double，64 位句柄在部分序列化器上会丢精度。
- `hwnds` 上限 32 项；重复项在加载时去重。
- 无效句柄（`IsWindow` 为假）在每次加载与每次切换前清理；清理后为空的组保留（用户可能只是暂时关了窗口），但 UI 上标灰。

**切换语义（ADR-0004 的落地细则）**

点击组 G：

1. 对 G 内每个有效 `hwnd` 调 `ShowWindow(hwnd, SW_RESTORE)`（若 `IsIconic`），再 `SetForegroundWindow(hwnd)`。
2. 对**不属于 G** 的、由本应用管理过的其他组的窗口调 `ShowWindow(SW_MINIMIZE)`。
3. 不改变 Z-order，不拦截 `Alt+Tab`，不装 `WH_CBT`。

**前台权限**

`SetForegroundWindow` 受前台锁定限制，失败时按顺序回退：

1. `AllowSetForegroundWindow(ASFW_ANY)` 后重试一次。
2. `AttachThreadInput(GetCurrentThreadId(), GetWindowThreadProcessId(target), TRUE)` + `SetForegroundWindow` + 立即 `AttachThreadInput(..., FALSE)`。
3. 仍失败则 `FlashWindowEx(hwnd, FLASHW_TRAY, 3, 0)` 提示用户，并记 WARN。

三条路径必须在实机上各自验证一次并把结果写进 `COMPATIBILITY.md`；不得只实现第一条就宣称可用。

**与 Dock 的联动**

Dock 右侧的 Stage Manager 入口是普通 tile，点击发事件；Stage Manager 模块订阅事件后显示/隐藏分组条。**Dock 不直接持有 Stage Manager 对象**（C-06 边界）。

## C-20 设置窗口与主进程事件（P1 冻结）

**事件名与载荷**

| 事件 | 载荷 | 发布者 | 订阅者 |
|---|---|---|---|
| `ConfigChanged` | 完整 `core::Config` 快照 | 配置热重载模块 | Dock、Launchpad、设置窗口自身 |
| `ThemeChanged` | `{ std::wstring name; }` | 主题模块 | Dock、Launchpad、Finder Bar |
| `TaskbarVisibilityChanged` | `{ bool visible; }` | 任务栏显隐模块 | 设置窗口 |

**载荷形态冻结为「完整快照」**，不用「变更键值对」。理由：三个订阅者都需要重新算布局，部分更新会让它们各自维护合并逻辑，而配置对象很小（约 1KB），整份复制的成本可以忽略。此条一旦冻结，不得混用两种形态。

**写入路径**

设置窗口的每一次修改 → 300ms 防抖 → `core::SaveConfig` → 发布 `ConfigChanged`。**只有主进程写配置文件**；设置窗口如果作为同进程内的独立 HWND 存在，则直接调 `SaveConfig`；如果将来独立成进程，则改为通过命名管道请求主进程写。当前实现取前者。

**非法输入**

设置窗口在提交前用 `core::ClampConfig` 钳制；钳制后的值与原值不同时，UI 立即回显钳制结果并 WARN，不静默接受。这保证「UI 显示的值 = 磁盘上的值」。

## C-21 材质降级事件（P1 冻结）

| 事件 | 载荷 | 触发条件 |
|---|---|---|
| `MaterialDegraded` | `{ core::MaterialLevel from; core::MaterialLevel to; const char* reason; }` | 省电模式开启、或 D3D 设备丢失、或捕获不可用 |
| `MaterialRestored` | `{ core::MaterialLevel to; }` | 上述条件全部解除 |

**省电检测**：`GetSystemPowerStatus` 的 `SYSTEM_POWER_STATUS.SystemStatusFlag == 1`（电池保护模式）。轮询间隔 5s，只在状态翻转时发事件，不每帧查。

**降级顺序**（沿用 ADR-0008）：`mica → acrylic → blur → solid`，只降不升，`MaterialRestored` 时一次性恢复到用户配置的等级。

**所有使用材质的窗口必须订阅**，不得各自轮询电源状态——否则同一台机器上不同窗口会在不同帧降级，视觉上割裂。
