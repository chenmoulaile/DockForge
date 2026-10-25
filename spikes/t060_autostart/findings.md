# T-060 spike · schtasks 参数路径 vs COM ITaskService 实测

日期：2026-10-25　环境：Windows 10.0.26300 x64，进程**非提升**（`net session` → NOT-ELEVATED，
`WindowsPrincipal.IsInRole('S-1-5-32-544')` → False）。当前 shell 为 Git Bash，已用
`MSYS2_ARG_CONV_EXCL="*"` 禁掉 MSYS 路径转换后重跑全部命令。

## 结论（先说）

| 路径 | 非提升可用？ | 实测证据 |
|---|---|---|
| `schtasks /create /sc onlogon`（参数路径，含 /rl limited、/ru 变体） | **否**，Access denied | 三次全部 rc=1，`错误: 拒绝访问。` |
| `schtasks /create /xml`（XML 注册路径，LogonTrigger + InteractiveToken + LeastPrivilege） | **可用** | `成功: 成功创建计划任务 "DockForge_Spike_T060_XML"。` rc=0 |
| COM `ITaskService` → `ITaskFolder::RegisterTaskDefinition`（MinGW `taskschd.h` + `libtaskschd.a` 存在） | **可用** | com_spike.exe 全链路 hr=S_OK（注册/GetTask/get_Xml/DeleteTask），spike rc=0 |
| `schtasks /query /delete`（对已有任务） | **可用** | daily 控制组：创建/查询/删除 rc=0 |

**决策**：主实现走 **纯 COM ITaskService**（`ITaskFolder::RegisterTaskDefinition`，
LogonTrigger + `TASK_LOGON_INTERACTIVE_TOKEN` + 当前用户）。理由：创建/查询/删除/导出 XML
四件事一条路径内闭环（`GetTask` / `DeleteTask` / `get_Xml`），不依赖临时文件，spike 已全链路
实测通过。`schtasks /create /sc onlogon` 在非提升进程**不可用**，工具链提示里的
"schtasks /create 封装"恰好落在被拒路径上——这是本次 spike 禁假设要求抓出来的事实。

## 实测命令与原文输出

### 1. schtasks 参数路径（onlogon）——三次全拒

```
$ schtasks /create /tn "DockForge_Spike_T060" /tr "\"C:\Windows\System32\notepad.exe\"" /sc onlogon /f
错误: 拒绝访问。        rc=1

$ schtasks /create /tn "DockForge_Spike_T060" /tr "C:\Windows\System32\notepad.exe" /sc onlogon /rl limited /f
错误: 拒绝访问。        rc=1

$ schtasks /create /tn "DockForge_Spike_T060" /tr "C:\Windows\System32\notepad.exe" /sc onlogon /rl limited /ru "chenk" /f
错误: 拒绝访问。        rc=1
```

### 2. 控制组（daily）——schtasks 本身可用，拒绝特定于 onlogon 触发器

```
$ schtasks /create /tn "DockForge_Spike_T060_D" /tr "C:\Windows\System32\notepad.exe" /sc daily /st 00:00 /f
成功: 成功创建计划任务 "DockForge_Spike_T060_D"。        rc=0
$ schtasks /query /tn "DockForge_Spike_T060_D"
DockForge_Spike_T060_D   2026/10/26 0:00:00   就绪            rc=0
$ schtasks /delete /tn "DockForge_Spike_T060_D" /f
成功: 计划的任务 "DockForge_Spike_T060_D" 被成功删除。    rc=0
```

### 3. COM 路径（com_spike.cpp，本目录可复现）

```
$ g++ -std=c++20 -O0 -Wall -o com_spike.exe com_spike.cpp -lole32 -loleaut32 -luuid -ltaskschd
$ ./com_spike.exe
== COM ITaskService spike (T-060) ==
  CoCreateInstance(CLSID_TaskScheduler) hr=0x00000000 (S_OK)
  Connect(local, current user)       hr=0x00000000 (S_OK)
  NewTask(0)                         hr=0x00000000 (S_OK)
  trigger=LogonTrigger user=chenk
  GetFolder(root)                    hr=0x00000000 (S_OK)
  RegisterTaskDefinition(root, logon) hr=0x00000000 (S_OK)
  OpenTask(verify)                   hr=0x00000000 (S_OK)
  xml head: <?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schem
  DeleteTask(cleanup)                hr=0x00000000 (S_OK)
== result: COM PATH WORKS ==   rc=0
```

### 4. XML round-trip（次要证据：schtasks /xml 也可注册 logon 触发器）

`./com_spike.exe keep` 留下任务 → `schtasks /query /tn DockForge_Spike_T060_COM /xml` 导出
（本目录 spike_task.xml，内容见上，Principal LogonType=InteractiveToken、Triggers 含
LogonTrigger UserId=CHENKX\chenk）→ `schtasks /create /tn "DockForge_Spike_T060_XML" /xml spike_task.xml`
→ `成功: 成功创建计划任务 "DockForge_Spike_T060_XML"。` rc=0。
注：schtasks 经管道导出的 XML 声明写 `encoding="UTF-16"` 实际是 ASCII 字节（schtasks 已知
怪癖），COM `get_Xml` 返回的才是真 UTF-16。

## 清理

四个 spike 任务（`DockForge_Spike_T060`/`_COM`/`_XML`/`_D`）全部已删除，复查
`schtasks /query /tn ...` 四次均 `错误: 系统找不到指定的文件。`，机器无残留。

## 实现注记（给 autostart.cpp）

- 注册：`RegisterTaskDefinition(L"\\DockForge", def, TASK_CREATE_OR_UPDATE,
  variant(当前用户), variant(empty), TASK_LOGON_INTERACTIVE_TOKEN, variant(empty), nullptr)`，
  挂在根文件夹的 `ITaskFolder` 上；`RegisterTaskDefinition` 在 `ITaskFolder` 而非
  `ITaskService`（MinGW 头里 ITaskService 没有该方法，spike 第一版编译已验证）。
- 触发器：`ITriggerCollection::Create(TASK_TRIGGER_LOGON)` → `ILogonTrigger::put_UserId(当前用户)`，
  只在该用户登录时触发（spike XML 的 `<LogonTrigger><UserId>` 可证）。
- principal：`UserId` = 当前用户 SID，`LogonType=InteractiveToken`，RunLevel 默认
  LeastPrivilege（非提升注册成功的前提，spike XML 可证）。
- MinGW 头/库：`/d/MSYS2/mingw64/include/taskschd.h`（含 CLSID_TaskScheduler 的
  DEFINE_GUID）、`libtaskschd.a` 存在；链接 `-lole32 -loleaut32 -luuid -ltaskschd`。
- MinGW 头的成员名注意：`IRegisteredTask::get_Xml`（非 get_XML）、`ITaskFolder::GetTask`
  （非 OpenTask），CMakeLists 的 `WIN32_LEAN_AND_MEAN` 不影响 taskschd.h。
