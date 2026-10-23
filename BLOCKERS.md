# BLOCKERS.md · 阻塞清单

> 每条：现象 / 已尝试 / 重试次数 / 所需帮助。同任务重试 ≥3 次标 BLOCKED。

## B-001 · GitHub remote 未配置
- **状态**：OPEN（不阻塞本地开发）
- **现象**：`gh auth status` → `You are not logged into any GitHub hosts`；仓库无 `origin`。
- **已尝试**：`gh auth status`（失败）；网络可达性已验证（`https://api.github.com` 返回 200）。
- **重试次数**：1
- **影响**：所有任务 commit 正常，push 步骤 SKIPPED 并在 STATE.json 记录。
- **所需帮助**：人类执行 `gh auth login`（或提供已有 remote URL / PAT），之后单条命令即可补齐：
  `gh repo create DockForge --private --source=. --remote=origin --push`

## B-002 · 无 MSVC / Windows SDK / Windows App SDK
- **状态**：RESOLVED（ADR-0002）
- **现象**：`cl`、`msbuild`、`vswhere` 全部缺失；`C:\Program Files (x86)\Windows Kits\10\Include` 为空目录。
- **处置**：改用 mingw-w64 工具链 + 纯 Win32/DComp/D2D，放弃 WinUI 3。已由 ADR-0002 记录。
- **残余影响**：MSIX 打包任务（P2）暂不可做，需 MSVC + Windows SDK；Inno Setup 便携版不受影响。

## B-003 · vcpkg 缺失
- **状态**：RESOLVED（ADR-0003）
- **处置**：第三方库就地 vendoring 到 `third_party/`。
