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

## B-004 · Windows SDK 工具链缺失，MSIX 打包不可执行
- **状态**：OPEN（阻塞 T-082）
- **现象**：`makeappx.exe` 与 `signtool.exe` 全盘零命中；`C:\Program Files (x86)\Windows Kits\10\bin` 为空，`Include` 目录不存在。`C:\Program Files (x86)\Windows Kits\10` 下只有 `Assessment and Deployment Kit` 与 `Catalogs` 两个目录。
- **已尝试**：全盘 `Get-ChildItem -Recurse -Filter makeappx.exe` 与 `signtool.exe`（均零命中）；检查 `Program Files` 与 `Program Files (x86)` 两处 SDK 根。
- **重试次数**：1
- **影响**：T-082（MSIX + `.appinstaller`）状态记 `BLOCKED`。T-083（Inno Setup 便携版）不受影响，已解耦。
- **所需帮助**：安装 Windows SDK 之一：
  `winget install Microsoft.WindowsSDK.10.0.26100`（约 3GB），或安装 VS Build Tools 勾选 Windows SDK。
  安装属环境变更，需人类确认后再执行。安装完成后 T-082 可直接解阻。

## B-005 · 应用无 UI Automation 提供者，FlaUI 无法定位元素
- **状态**：OPEN（阻塞 T-080）
- **现象**：`src/**` 全量 grep `WM_GETOBJECT` / `UIAutomation` / `IAccessible` / `AutomationId` 零命中。应用是纯 Win32 + DComp 自绘，没有任何元素暴露给 UIA，FlaUI 只能看到顶层 HWND，看不到 tile 或控件。
- **已尝试**：全仓源码 grep；确认 .NET 8.0.426 SDK 与 NuGet（`https://api.nuget.org/v3/index.json` 返回 200）均可用，所以阻塞点不在测试侧。
- **重试次数**：1
- **影响**：T-080 状态记 `BLOCKED`。已新增前置任务 T-079（UIA 提供者）承担该工作。
- **所需帮助**：无。T-079 属可自决的本地实现，不需人类介入。
