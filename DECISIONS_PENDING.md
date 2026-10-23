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
