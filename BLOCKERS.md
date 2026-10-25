# BLOCKERS.md · 阻塞清单

> 每条：现象 / 已尝试 / 重试次数 / 所需帮助。同任务重试 ≥3 次标 BLOCKED。

## B-001 · GitHub remote 未配置
- **状态**：OPEN（不阻塞本地开发）
- **现象**：`gh auth status` → `You are not logged into any GitHub hosts`；仓库无 `origin`。
- **已尝试**：`gh auth status`（失败）；网络可达性已验证（`https://api.github.com` 返回 200）。
- **重试次数**：1
- **影响**：所有任务 commit 正常，push 步骤 SKIPPED 并在 STATE.json 记录。
- **所需帮助**：人类执行 `gh auth login`（或提供已有 remote URL / PAT），之后单条命令即可补齐。
- **更新（2026-10-25）**：用户要求"新开一个公开 GitHub 项目并推送"。当前 `gh auth status`
  仍为未登录，Agent 无法代办（登录是交互式流程）。解锁只需一步：
  **`gh auth login`（在终端里按提示走浏览器授权）**，然后 Agent 或用户执行：
  `gh repo create DockForge --public --source=. --remote=origin --push`
  （若想要私有仓库把 --public 换成 --private；推送内容已含全部源码/测试/证据/文档，
  evidence/ 与 spikes/ 是否公开由 P3 的 C-10 排除清单在正式发布前再清理）。

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

## B-006 · UserNotificationListener 对消息计数场景覆盖不成立（T-065 DoD #1）
- **状态**：OPEN → 已按 DoD 降级条款处置（不阻塞 T-065 主体交付）
- **现象**：`UserNotificationListener`（WinRT）在非打包进程 API 完全可用
  （`spikes/T-065_notification_probe/`：.NET 投影实测 access=Allowed、51 条
  通知可枚举；MinGW 直连 spike 同步路径闭环），但通知中心里**微信 / QQ 均为
  0 条**——聊天应用的未读消息不进通知中心持久层，只有未处理 toast 的短暂
  窗口期可见。作为"未读消息计数"来源对目标应用没有覆盖。
- **已尝试**：双探针（.NET 投影 + MinGW RoGetActivationFactory 直连）；
  RequestAccessAsync/GetAccessStatus/GetNotificationsAsync 全链路；枚举结果
  按 AppId 聚合比对 weixin|wechat|qq|tencent。
- **重试次数**：1（spike 一次性定性）
- **影响**：T-065 按 ADR-0022 走逐应用适配器（C-25）：窗口标题未读数适配器
  （微信/QQ 主窗口标题 `(N) …`，应用自身可观测面，不 hook、不读聊天内容）。
  `UserNotificationListener` 完整链路留给 T-076（通知中心），那里是正确数据源。
- **所需帮助**：无。

## B-007 · WinRT async（GSMTC / Radios）在 MinGW 进程内永不完成（T-068）
- **状态**：OPEN → 已按 ADR-0002 的既有边界走 .NET sidecar 降级（不阻塞 T-068 交付）
- **现象**：`Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager.RequestAsync()`
  与 `Windows.Devices.Radios.Radio.GetRadiosAsync()` 在 MinGW-w64 直连
  （`RoGetActivationFactory` 动态解析，combase.dll）下返回合法
  `IAsyncOperation`，但状态恒为 Started：15 秒轮询不完成、注册的
  completion handler 从不触发（含 MTA、消息泵、put_Completed 三种等待方式）。
  与 T-065 记录的 console 进程 async 边界一致——这是 WinRT async 完成机制
  与本进程形态的边界，不是 MinGW 编译边界（同一调用在 .NET 投影下 ~1s 完成）。
- **已尝试**：`spikes/T-068_hw_probe/winrt_mingw.cpp`（GSMTC Statics IID
  {2050C4EE-11A0-57DE-AED7-C97C70338245}、Radio Statics IID
  {5FB6A12E-67CB-46AE-AAE9-65919F86EFF4}，均 GetIids 实测发现；两路 async
  各等 15s）；`.NET 投影 winrt_probe.cs` 对照（两路 async 均 Completed，
  Chrome 会话 title/timeline、蓝牙+WLAN 无线电全可读）。
- **重试次数**：1（spike 一次性定性）
- **影响**：T-068 的媒体控制与 WiFi/蓝牙开关不能在主进程内直连实现，
  改为 `tools/hw_sidecar.cs`（.NET Framework 自带 csc 编译，无 SDK 依赖）
  子进程 + stdin/stdout 行协议（`src/finderbar/media_sidecar.cpp` 桥）。
  sidecar 随面板开/关起/停，无孤儿进程（T-068 probe G 部分实证）。
  C++/WinRT 工具链（ADR-0002 已记本机无 MSVC）恢复后可收回 sidecar。
- **所需帮助**：无。
