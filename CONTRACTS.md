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
