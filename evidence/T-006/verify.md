# T-006 验证报告 · shell: 图标提取 + .lnk/.exe/UWP 解析

日期：2026-10-24
提交：见 `git log -1 --format=%H`
构建：`tools/build.ps1 -Action all`（MinGW g++ 15.2.0，C++20）

## DoD 逐条核对

| # | DoD | 结论 | 证据 |
|---|---|---|---|
| 1 | `IShellItemImageFactory::GetImage` 提取 256px PNG 缓存 | PASS | `run_icon_*.txt`、`icon_*_256.png`、`tests/shell_icon_test.cpp` |
| 2 | `.lnk` 通过 `IShellLinkW` 解析目标与参数 | PASS | `run_lnk_first.txt`、`run_lnk_batch.txt`、`run_expand_admin.txt` |
| 3 | UWP 通过 `ApplicationUserModelID` 解析，回退到 `.lnk` | PASS（回退策略见下） | `run_uwp_calculator.txt`、`run_appsfolder*.txt` |
| 4 | 缓存命中率与耗时打点（冷/热） | PASS | `coldstart.txt`、`tests/shell_icon_test.cpp` |
| 5 | 单测：路径不存在返回明确错误码而非崩溃 | PASS | `test.txt`（295 assertions / 58 cases） |

## 测试

```
All tests passed (295 assertions in 58 test cases)
```

shell 相关用例覆盖：五形态分类、命名空间子项构造与前缀去重、图标缓存键稳定性、
磁盘往返（新实例 `disk_hits==1 && extractions==0`）、不可解析源 `failures==1`、
`Launch` 的 `NotLaunchable`/`NotFound` 分支与 `route` 字段。

## AppsFolder 枚举：215 项，五形态

```
INFO [shell] AppsFolder scan: 215 entries (shortcut=49 uwp=43 desktop-id=69 namespace=54 skipped=1)
INFO [app]   apps: 215 (shortcut=49 uwp=43 desktop-id=69 namespace=54 skipped=1) scan=453.6ms icons=24/24 in 20.8ms
```

| 形态 | 数量 | 样例 | 启动路径 |
|---|---|---|---|
| `ShortcutPath` | 49 | `C:\...\AutoHotkey Dash.lnk` | `IShellLinkW` → `CreateProcessW` |
| `PackagedAppId` | 43 | `Microsoft.WindowsCalculator_8wekyb3d8bbwe!App` | `ActivateApplication` |
| `DesktopAppId` | 69 | `Anysphere.Cursor`、`Chrome` | `ShellExecuteEx("shell:AppsFolder\...")` |
| `NamespacePath` | 54 | `{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\cmd.exe` | `ShellExecuteEx("shell:AppsFolder\...")` |
| 跳过 | 1 | `D:\Beautiful\.tools\innosetup\Examples`（目录不存在） | — |

上一版把 124 项全丢进 `skipped` 是分类错误：其中 69 个裸 id 与 54 个命名空间子项都是
合法条目，只是没有可执行文件路径。`SHGetPathFromIDListW` 对 54 个命名空间子项
**0/54 成功**，证明它们确实无文件系统路径，不能按路径分类。

## 三条实测硬约束

1. **命名空间子项必须先加 `shell:AppsFolder\` 前缀**

   | 输入 | API | 结果 |
   |---|---|---|
   | `{GUID}\cmd.exe` | `SHCreateItemFromParsingName` | `0x80070002` |
   | `{GUID}\cmd.exe` | `SHParseDisplayName` | `0x80070002` |
   | `::{GUID}\cmd.exe` | `SHParseDisplayName` | `0x80070057 E_INVALIDARG` |
   | `shell:AppsFolder\{GUID}\cmd.exe` | `SHParseDisplayName` | **S_OK**，图标 `ns_cmd_64.png` |

2. **裸 `DesktopAppId` 不能用 `ActivateApplication`**：`0x80070057 E_INVALIDARG`；
   `ShellExecuteExW("shell:AppsFolder\<id>")` 成功。

3. **`IContextMenu::InvokeCommand` 只能按菜单序号**：`MAKEINTRESOURCEA(index-1)` 成功，
   verb 字符串 + `CMIC_MASK_UNICODE` 返回 `E_INVALIDARG`。

## 环境变量展开是必需的

```
raw            =%windir%\system32\control.exe
expanded       =C:\Windows\system32\control.exe
exists_raw     =0
exists_expanded=1
SHCreateItem(raw)      hr=0x80070002
SHCreateItem(expanded) hr=0x00000000
```

`IShellLinkW::GetPath(SLGP_RAWPATH)` 返回未展开的 `%windir%` 形式，直接喂给 shell 必然失败。
`ResolveShortcut` 对 `target_path` / `arguments` / `icon_path` / `working_dir` 全部调用
`ExpandEnvironmentStringsW`。

## 冷/热图标缓存

删除 `%LOCALAPPDATA%\DockForge\icons` 后连续两次启动（同一批 24 个图标预热）：

| 场景 | 扫描 | 图标预热 |
|---|---|---|
| 冷（无磁盘缓存） | 495.9 ms | **553.5 ms** |
| 热（24 张 PNG 已落盘） | 483.2 ms | **30.8 ms** |

磁盘占用 24 个 PNG / 534012 字节（平均 22 KB @ 256px）。热启动快 18 倍。

单张提取耗时（spike 实测）：冷 57–190 ms，热（shell 自身缓存）1.6–5.5 ms。

## 冷启动

从进程启动到窗口可见（`EnumWindows` 轮询 `IsWindowVisible`）：**642 ms**（预算 800 ms，PASS）。
其中 AppsFolder 扫描 453.6 ms 是主要成本，已记入后续优化任务。

## 渲染验证（三档 DPI）

`tools/alpha_probe.ps1` 差分采样（杀进程取基线，排除 DWM 异步假象）：

| DPI | 物理尺寸 | painted | transparent | 结论 |
|---|---|---|---|---|
| 96 | 528x152 | 4 | 2 | PASS |
| 144 | 792x228 | 4 | 2 | PASS |
| 192 | 1056x304 | 4 | 2 | PASS |

每档 `center`/`top_mid`/`left_mid`/`edge_bottom` 判 PAINTED，`corner_tl`/`corner_br` 判 TRANSPARENT。

截图 `icons_dpi{96,144,192}.png` 用 `tools/sample_pixels.ps1 -Image` 从文件本身采样
（`px_icons_dpi*.txt`），与差分结论一致：center `236,240,248`（标签白字），
top_mid `13,15,20`（板色），corner `1,0,5`（透明区露出桌面）。

## 性能

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| 空闲 CPU（90s，solid） | 0.031 s / 0.0019% | < 0.5% | PASS |
| 空闲 RSS | 88.57 → 88.52 MB | < 120 MB | PASS |
| 句柄（90s） | 371 → 371 | 无增长 | PASS |
| 帧 avg / p50 | 16.00 / 16.01 ms | — | PASS |
| 帧 p99（20 个采样窗） | mean 16.718，max 17.08，5/20 窗 > 16.7 | < 16.7 ms | **WATCH** |
| 冷启动到可见 | 642 ms | < 800 ms | PASS |

p99 超预算的成因是消息循环尾延迟（p50 稳定在 16.01，avg 稳定在 16.00），不是绘制变慢。
已拆出优化任务，见 `TASKS.md` T-006a。

## 工具修正（本轮）

`tools/sample_pixels.ps1` 原实现**忽略 `-Image` 参数**，永远抓实时屏幕。这会让
「报告说 A、文件是 B」——用截图当证据时等于伪造。已改为：
`-Image` 给定时从文件读取并支持 `-Rect` 作为文件内裁剪偏移；不给时才抓屏。
本轮三张 DPI 截图的重采样因此与旧报告不同（旧报告采的是屏幕而非文件）。

`tools/fix_encoding.ps1` 新增：检测并修复 UTF-8/GBK 混合编码。
`DECISIONS.md`、`PERFORMANCE.md`、`PROGRESS.md` 曾被 PowerShell 5.1 的
`Set-Content` 默认 ANSI 代码页写坏（中文尾部变成 GBK），已修复并验证全部状态文件为合法 UTF-8。
