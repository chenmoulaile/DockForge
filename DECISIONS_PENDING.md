# DECISIONS_PENDING.md · 待人类决策

> 当前无阻塞性待决策项。以下为「知会级」记录，Agent 已按 DECISION AUTONOMY 自决并继续。

## 知会-01 · 放弃 WinUI 3（已自决，可回退）
- 决策：ADR-0002，纯 Win32 + DirectComposition + Direct2D。
- 触发条件：本机无 MSVC / 无 Windows SDK / 无 Windows App SDK，WinUI 3 无法构建。
- 授权依据：ADR-0001 回退条款明确允许，且属「可自决变更」。
- 如需推翻：安装 VS 2022 (Desktop C++) + Windows SDK + Windows App SDK 后，可新开 ADR 迁回。

## 知会-02 · 分发形态降级（已自决）
- 原计划 MSIX 主分发；因缺 MSVC/SDK，P2 阶段先做 Inno Setup 便携版，MSIX 顺延至工具链补齐后。
- 不影响 P0/P1 任何功能。

## 知会-03 · GitHub 远程暂缺（需人类一次性动作，不阻塞）
- 见 BLOCKERS.md B-001。人类执行 `gh auth login` 即可，Agent 下一轮自动补建私有仓库并推送。

## 知会-04 · P1/P2 任务书按仓库真实栈执行（已自决，可回退）
- 决策：ADR-0021。P1/P2 的 Launchpad、设置窗口、Stage Manager 全部用纯 Win32 + DirectComposition + Direct2D 实现，不引入 WinUI 3 / Windows App SDK。
- 触发条件：任务书假设 WinUI 3 + Windows App SDK，但本机无 MSVC（`vswhere.exe` 不存在）、无 Windows SDK 头文件与工具（`makeappx.exe` / `signtool.exe` 全盘零命中）、无 Inno Setup（`iscc.exe` 缺失）。ADR-0002 已冻结纯 Win32 栈。
- 连带调整：新契约从 C-18 起编（任务书的 C-05..C-08 与仓库既有编号冲突），新 ADR 从 ADR-0021 起编（任务书的 ADR-0003..0008 同样冲突）。
- 阻塞移交：T-082（MSIX）→ `BLOCKED`，见 B-004；T-080（FlaUI）→ `BLOCKED`，见 B-005，前置任务 T-079 承担 UIA 提供者实现。
- 另修正一处任务书前提：任务书称「上一阶段完成 P0：Dock 栏 + Finder Bar」，实际 `src/finderbar` 不存在，M2 Finder Bar 仍为 `TODO`。已完成范围是 M0 + M1（T-001..T-016）。
- 如需推翻：安装 VS 2022 (Desktop C++) + Windows SDK + Windows App SDK 后新开 ADR 迁回 XAML 方案，届时 Launchpad 与设置窗口需重写。

## 待决-01 · behavior.hide_taskbar 出厂默认是否在 T-312 发布冻结时翻为 true（视觉修复会话升级裁决，2026-11-18）
- 背景：验收以 D:/Beautiful 参考截图为基准，"Windows 11 任务栏完整可见"是整屏观感最大单点偏差（参考产品把原生任务栏移出屏幕）。隐藏能力 T-061 已完整实现（Shell_TrayWnd + Shell_SecondaryTrayWnd、异步 settle、设置开关/Ctrl+Alt+T/正常退出/崩溃过滤器四路恢复，evidence/T-061/）。
- 升级裁决（主控批复）：C-01 冻结默认 false **不改**——PROMPT.md 第 9 条「改变系统行为的功能默认关闭」与 T-312 发布冻结 DoD「默认安全」是发布级裁决，不属于本修复冲刺。
- 本轮已执行：本机 config.json 的 behavior.hide_taskbar 已设 true（用户最高指令覆盖本机观感，一键可还原：改回 false 或托盘/设置开关）；整屏取证按此真实效果评。
- 待决：T-312（发布冻结）时是否把默认翻为 true。证据：本会话整屏对比截图（隐藏前后）；还原路径齐备（四路恢复均实证）。
- 决策人动作：在 T-312 开工时裁决 ADR（翻/不翻），并把裁决结果同步 C-01 表与迁移说明。
