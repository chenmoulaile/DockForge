# T-065 spike · UserNotificationListener 可行性实测

日期：2026-10-25。两个互补探针，日志都在 `evidence/T-065/`。

## 1. notif_probe.cs / notif_probe.exe —— 官方 .NET 投影（可见性验证）

构建（本机无 Windows SDK，用 .NET Framework 自带 csc + 系统 winmd；
PowerShell 5.1 的 Add-Type 不能传 winmd 引用，故手动 csc）：

```
C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo -target:exe \
  -out:notif_probe.exe -r:System.Runtime.WindowsRuntime.dll \
  -r:$F64/System.Runtime.dll \
  -r:C:/Windows/System32/WinMetadata/Windows.UI.winmd \
  -r:C:/Windows/System32/WinMetadata/Windows.Foundation.winmd notif_probe.cs
```

结果（`evidence/T-065/spike_usernotif_probe.log`）：

- `UserNotificationListener.Current` 在**非打包桌面进程**激活 OK。
- `GetAccessStatus()`（同步）= **Allowed**。
- `GetNotificationsAsync(Toast)` 完成，**51 条通知、11 个 AppId 可枚举**
  （ai.opencode.desktop、Chrome、dev.zcode.app 等）。
- **微信 / QQ 当前均为 0 条**：聊天应用的未读消息不会持久停留在通知中心，
  只有"toast 已发出且未被清走"的窗口期才可见 → 作为**未读消息计数**来源，
  UserNotificationListener 对目标应用（微信 UWP / QQ Win32 / Discord）没有
  可用的覆盖。
- 附带拿到 ABI 声明序（= vtable 序，spike.cpp 的依据）：
  - `IUserNotificationListener`：RequestAccessAsync, GetAccessStatus,
    add/remove_NotificationChanged, GetNotificationsAsync, GetNotification,
    ClearNotifications, RemoveNotification（注意 Clear 在 Remove 之前）。
  - `IUserNotification`（本 OS build 26300）：get_Notification, get_AppInfo,
    get_Id, get_CreationTime —— **AppId 成员已从契约移除**（被 AppInfo 取代）。

## 2. spike.cpp / spike.exe —— MinGW-w64 直连（无 C++/WinRT、无 MSVC）

```
g++ -std=c++17 -O0 -g -o spike.exe spike.cpp -lole32
```

结果（`evidence/T-065/spike_mingw_direct.log`）：

- `LoadLibrary("combase.dll")` 动态解析 Ro*（MinGW 无对应导入库，
  `nm libole32.a`/`libruntimeobject.a` 均无 `RoGetActivationFactory`）。
- `RoGetActivationFactory` 激活 OK —— 注意第一参数必须是 **HSTRING**
  （`WindowsCreateString`），传裸宽字符串返回 E_INVALIDARG（第一次运行的实测）。
- IID 不靠记忆手写：factory 的 `GetIids()` 实测返回
  `{FF6123CF-4386-4AA3-B73D-B804E5B63B23}`，与 windows-rs 官方生成的
  `IUserNotificationListenerStatics` IID 一致；listener 的 GetIids 证实
  `{62553E41-8A06-4CEF-8215-6033A5BE4B03}` = `IUserNotificationListener`。
  （`GetRuntimeClassName` 在 factory 上返回 E_NOTIMPL，名称匹配不可用。）
- `get_Current` → listener 实例 OK；**`GetAccessStatus()` 同步返回 Allowed**
  —— MinGW 直连 WinRT 的同步路径 100% 闭环。
- 边界（实测，非推测）：`GetNotificationsAsync` 的 async 操作在本 console
  进程 MTA/STA 下均不完成（状态恒 Started）；同一调用在 .NET 投影
  （AsTask 桥接 apartment）约 1s 完成。async 完成机制是 console 进程边界，
  不是 MinGW 边界。Dock 主实现不依赖该 async 路径。

## 结论（按 DoD 与 ADR-0022 的降级条款）

1. `UserNotificationListener` **API 可用**（.NET 与 MinGW 直连双路径证实），
   但对消息计数场景**覆盖不成立**：微信/QQ 的未读消息不进通知中心持久层。
   → 记入 `BLOCKERS.md`（B-006），按 ADR-0022 走逐应用适配器。
2. 主实现 = C-25 适配器框架 + **窗口标题未读数适配器**
   （微信/QQ 主窗口标题形如 `(N) 微信`，是应用自己的可观测面，ADR-0022
   明确允许；不 hook 进程、不读聊天内容）。
3. `UserNotificationListener` 完整链路留给 T-076（通知中心），那里它本来就是
   正确的数据源。
