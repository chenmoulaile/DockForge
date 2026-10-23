# T-011 验证报告 · dock 点击行为

## DoD 核对

| # | 条目 | 状态 | 证据 |
|---|---|---|---|
| 1 | 未运行 → 启动 | 通过 | `click_uwp.txt`（`route=launch` 374.58ms 首次）、`click_bench.txt`（`action_launched=1`） |
| 2 | 前台 → 最小化；后台 → 激活到前台 | 通过 | `click_tile0.txt`（Notepad → 7zFM → 二次点击回 Notepad 且 `previous_foreground_iconic=True`）、`click_tile4.txt`（AutoHotkey Help） |
| 3 | 激活失败（UWP/权限）有降级路径且不崩溃 | 通过 | `activate.txt`（裸 `SetForegroundWindow` 与 `AttachThreadInput` 均被拒）、`fg_altkey.txt`（合成 ALT 成功）、`fg_click.txt`（点击路径直接成功） |
| 4 | 点击热区与视觉一致（含放大态） | 通过 | `HitTest` 读当前悬停布局；`click_probe.ps1` 的 `window_from_point=True` 与命中槽位一致 |
| 5 | 日志记录每次动作与耗时 | 通过 | 日志行 `click 计算器 -> activated (pid=29520 route=raise alt=0 in 0.52ms)` |

## spike 结论（`spikes/t011_click/`）

### 窗口筛选（`enum.txt`）

空闲桌面 `total_windows=198`、`app_windows=0`；开一个 Notepad 后 `total_windows=205`、`app_windows=1`。

五条筛选条件全部必要：`IsWindowVisible` && 无 owner && 非 `WS_EX_TOOLWINDOW` && `DWMWA_CLOAKED==0` && 标题非空。`IsWindowVisible` 单独用会放进 `ApplicationFrameWindow`、`DummyDWMListenerWindow`、`ForegroundStaging`、`tooltips_class32`。

### 打包应用归属（`frame.txt`、`aumid.txt`）

Calculator 的顶层窗口 `pid=16276` 是 `ApplicationFrameHost.exe`，`GetApplicationUserModelId` 对它返回 `0x00003D57`（`ERROR_NOT_FOUND`）。`EnumChildWindows` 找到跨进程子窗口 `pid=17292`（`CalculatorApp.exe`），后者才报 `Microsoft.WindowsCalculator_8wekyb3d8bbwe!App`。

未运行的 frame 窗口 `cloaked=1`，没有跨进程子窗口。

### 前台权限（`fg_plain.txt`、`fg_altkey.txt`、`fg_click.txt`）

| 路径 | 结果 |
|---|---|
| `SetForegroundWindow` 无输入 | `verdict=REFUSED` |
| `AttachThreadInput` 后重试 | `verdict_attach=STILL_REFUSED` |
| 合成 ALT 后重试 | `verdict=ALTKEY_ACTIVATED` |
| 在 `WS_EX_NOACTIVATE` 窗口的 `WM_LBUTTONDOWN` 内调用 | `verdict=CLICK_GRANTS_FOREGROUND` |

结论：点击处理函数本身持有前台权限，不需要 `AttachThreadInput`，也不需要预先合成 ALT。`alt=1` 出现在正常会话即缺陷信号。

## 实现

新增 `src/shell/window_registry.{h,cpp}`（五条筛选、`app_pid` 分离、AUMID 优先匹配、文件身份比较）、`src/shell/window_actions.{h,cpp}`（三分支状态机、`ShowWindowAsync`、ALT 回退）、`DockPainter::HitTest`、`DOCKFORGE_PIN_APPS` 验证钩子、`WM_LBUTTONDOWN` 接入。

## 四轮修正

| 轮次 | 现象 | 根因 | 修复 | 修复后 |
|---|---|---|---|---|
| 1 | 点击计算器激活了设置 | AUMID 与路径「或」匹配，frame host 路径相同 | AUMID 存在时只按 AUMID 匹配 | 单测 `prefers the AUMID over a coincidental path match` 通过 |
| 2 | 7-Zip 每次点击都启动新副本 | scoop junction 让路径字符串永不相等 | `GetFileInformationByHandle` 卷序列号 + 文件索引 | 日志变 `activated (raise, pid=30852)` |
| 3 | restore 被标成 raise | `IsIconic` 在动作后采样 | 动作前采样 | 日志变 `route=restore alt=0 in 18.66ms` |
| 4 | 点击 p99 494.59ms | `ShowWindow` 同步等待目标应用 | `ShowWindowAsync` | `avg=1.92 p99=9.89 超预算 0/20` |

`path_mismatch.txt` 是第 2 轮的直接证据：两个路径字符串不等、`exists` 都为真、文件长度相同（999424B）、`verdict=PATH_STRING_DIFFERS_FILE_IDENTITY_SAME`。

## 实测

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| 点击 p99（20 次） | 9.89ms | <16.7ms | PASS |
| 点击 avg | 1.92ms | — | PASS |
| 超预算次数 | 0 / 20 | 0 | PASS |
| 窗口枚举 | 0.37–0.70ms | <1ms | PASS |
| 空闲 CPU（60s） | 0.0029% | <0.5% | PASS |
| 空闲 RSS | 92.79MB | <120MB | PASS |
| 句柄（60s） | 380 → 372 | 无增长 | PASS |

测试：`All tests passed (1415 assertions in 112 test cases)`。

## 诚实边界

- 单显示器主机：`restore` 与 `minimize` 都在同一屏幕验证，跨屏激活未测。
- UWP 分支用「计算器」验证（`DOCKFORGE_PIN_APPS=calculator`），未覆盖所有打包应用；某些应用可能拒绝 `SetForegroundWindow` 而落到 ALT 回退路径。
- `route` 的 `restore`/`raise` 区分依赖动作前的 `IsIconic` 采样，采样与动作之间若被第三方最小化会标错，不影响行为。
- 点击热区与视觉一致性的验证是「命中测试用当前布局」这一结构性保证 + `window_from_point` 单点核对，未做逐像素可点击区域扫描。
