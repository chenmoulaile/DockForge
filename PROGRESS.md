# PROGRESS.md · 执行日志（倒序追加在末尾）

> 每任务追加一条。格式见 PROMPT.md · CLOSE PROTOCOL。

## 2026-02-11T00:00:00+08:00 | BOOTSTRAP | DONE
- 做了什么：环境勘察（无 MSVC/无 Windows SDK/无 vcpkg；有 MSYS2 g++ 15.2.0 + CMake 4.4.2 + mingw32-make）；建立目录骨架；落盘 11 个状态文件；`git init` 于 `D:\MacDock`，分支 `main`，身份 `DockForge Agent <dockforge@local>`。
- 关键决策：ADR-0002（改用纯 Win32 + DComp + D2D，依据原 ADR-0001 回退条款）；ADR-0003（vendoring nlohmann/json 3.11.3 + Catch2 3.5.4）。
- 证据：evidence/（待各任务填充）
- 遗留：gh CLI 未登录 → 无 GitHub remote，已记 BLOCKERS.md B-001；本地 commit 正常。
- 下轮开始于：T-001
