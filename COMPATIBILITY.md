# COMPATIBILITY.md · 兼容性矩阵

## 系统

| 版本 | 状态 | 备注 |
|---|---|---|
| Windows 10 21H2 | 待测 | Mica 不可用 → Acrylic |
| Windows 10 22H2 | 待测 | 同上 |
| Windows 11 22H2 | 待测 | Mica 可用 |
| Windows 11 23H2 | 待测 | |
| Windows 11 24H2 | 待测 | |
| Windows 11 26xxx (Insider) | 开发机 | 当前开发环境 |

## DPI

| 场景 | 状态 |
|---|---|
| 100% 单屏 | 待测 |
| 150% 单屏 | 待测（每 UI 任务必测） |
| 200% 单屏 | 待测 |
| 混合 DPI 双屏 | 待测（每 UI 任务必测） |

## 显示器布局

| 场景 | 状态 |
|---|---|
| 单屏 | 待测 |
| 双屏同分辨率 | 待测 |
| 双屏异分辨率 | 待测 |
| 竖屏 | 待测 |

## 已知限制

- 无 MSVC/Windows SDK：MSIX 打包暂不可做（见 BLOCKERS.md B-002）。
- 无 Windows App SDK：不使用 WinUI 3（见 DECISIONS.md ADR-0002）。
