# COMPATIBILITY.md · 兼容性矩阵

状态口径：**通过** = 有证据文件；**逻辑通过** = 纯逻辑单测覆盖但未在真机复现；
**待测** = 无证据；**不可测** = 当前硬件不具备条件。

## 系统

| 版本 | 状态 | 备注 |
|---|---|---|
| Windows 10 21H2 | 待测 | Mica 不可用 → Acrylic |
| Windows 10 22H2 | 待测 | 同上 |
| Windows 11 22H2 | 待测 | Mica 可用 |
| Windows 11 23H2 | 待测 | |
| Windows 11 24H2 | 待测 | |
| Windows 11 26xxx (Insider) | 通过 | 开发机 build 26300；`evidence/T-005`、`T-006`、`T-007` |

## DPI

开发机只有一块显示器（`\\.\DISPLAY1` 2880x1920，有效 192 DPI / 原始 243 DPI）。
三档 DPI 通过 `DOCKFORGE_DPI_OVERRIDE` 覆盖得到，属**进程内可控验证**，非真实换屏。

| 场景 | 状态 | 证据 |
|---|---|---|
| 100% 单屏 | 通过 | `evidence/T-007/dpi96.png`、`alpha96.txt`、`px_dpi96.txt` |
| 150% 单屏 | 通过 | `evidence/T-007/dpi144.png`、`alpha144.txt`、`px_dpi144.txt` |
| 200% 单屏 | 通过 | `evidence/T-007/dpi192.png`、`alpha192.txt`、`px_dpi192.txt` |
| 混合 DPI 双屏 | **逻辑通过**（不可真机测） | `tests/display_test.cpp` 的 96+144 用例；`simulate_dpi*.txt` |

## 显示器布局

| 场景 | 状态 | 证据 |
|---|---|---|
| 单屏 | 通过 | `evidence/T-007/edge_*.png` |
| 双屏同分辨率 | 不可测（单显示器） | — |
| 双屏异分辨率 | 不可测（单显示器） | — |
| 竖屏 | 不可测（单显示器） | — |
| 负原点副屏 | 逻辑通过 | `tests/display_test.cpp` 负坐标用例 |

## 任务栏显隐（T-061）

| 场景 | 状态 | 证据 |
|---|---|---|
| Win11 26300：ShowWindowAsync 显/隐、恢复、Explorer 重启自然恢复 | 通过 | `evidence/T-061/`（verify.txt verdict=PASS、A2/B1/C1/D2/E2/F2-F4 + 五张截图）；spike `spikes/t061_taskbar/`（显/隐 ~16ms 收敛；隐藏时 SPI_GETWORKAREA 不变） |
| Win10 21H2/22H2：Shell_TrayWnd 行为差异 | 待测（无 Win10 环境，NOT RUN） | `spikes/t061_taskbar/findings.md` 未测边界一节 |
| 多显示器 `Shell_SecondaryTrayWnd` | 不可测（单显示器，代码按枚举覆盖） | — |
| 任务栏"自动隐藏"模式交互 | 待测（本机未开自动隐藏） | — |

## 已知限制

- 无 MSVC/Windows SDK：MSIX 打包暂不可做（见 BLOCKERS.md B-002）。
- 无 Windows App SDK：不使用 WinUI 3（见 DECISIONS.md ADR-0002）。
- **单显示器主机**：真实跨屏 DPI 转换、双屏布局、竖屏均无法在本机复现。合成 `WM_DPICHANGED`
  只能验证处理器与放置数学（ADR-0012），不得据此外推为"混合 DPI 实测通过"。
- **DXGI Desktop Duplication 不可用**：`DuplicateOutput` 返回 `E_ACCESSDENIED`，本机有
  GameViewer / MuMu 两个虚拟显示适配器（见 `evidence/T-005/dupl_bench.txt`）。

## 消息计数徽章（T-065）

| 场景 | 状态 | 证据 |
|---|---|---|
| 标题徽章链路（窗口标题 → 适配器 → tile → 红点） | 通过 | `evidence/T-065/`（probe.log A/B/C/D 全 PASS，badge_on/off/reenabled/disabled 四张截图，像素判定 589/589/0） |
| 目标进程退出 → CountChanged(0) → 徽章消失 | 通过 | 同上（Part C + 单测 `tests/message_counts_test.cpp`） |
| 设置开关（热重载）单独禁用适配器 | 通过 | 同上（Part D：config 持久化 + 禁用后静默） |
| `UserNotificationListener` 作消息计数来源 | 不适用（见 BLOCKERS.md B-006） | `spikes/T-065_notification_probe/README.md`：API 可用（access=Allowed、51 条通知可枚举），但微信/QQ 未读消息不进通知中心持久层 |

## 已知渲染观察（本机）

- **自绘磨砂（blur 档）在本机当前呈现为空渐变**（2026-10-25 观察，T-065 探针期间）：
  dock 窗口存在、topmost、paint 循环正常，`DOCKFORGE_FORCE_MATERIAL=solid` 下 plate 与
  图标渲染完全正常（`evidence/T-065/dbg_solid.png`），默认 mica→blur 降级后板面内容缺失
  （`dbg_badge_on.png` 背景板仍带内容但 `dbg_badge_off2.png` 板面曾整板为渐变；DComp 呈现
  或 backdrop capture 链与本机 GPU 状态相关，仓库根有 fix-gpu.ps1 先例）。徽章/适配器
  逻辑不依赖该路径。证据探针一律用 `DOCKFORGE_FORCE_MATERIAL=solid` 取证。
