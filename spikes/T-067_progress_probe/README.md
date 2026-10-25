# T-067 spike · 任务栏进度读取路径实测

日期：2026-10-25。所有结论都来自本目录探针在本机的实际运行（日志：`self_run.txt`、`watch_run.txt`、`watch_run2.txt`、`watch_run3.txt`、`watch_random.txt`）。

## 背景问题（ADR-0024）

`ITaskbarList3::SetProgressValue/SetProgressState` 是**设置**接口。读取路径必须先证明，候选有三：目标窗口代理状态（window property）、vtable hook、白名单轮询。

## Q1 · 窗口属性代理状态：**不成立**（`self_run.txt`）

`spike.exe self`：自建窗口 → `SetProgressState(NORMAL)` + `SetProgressValue(37,100)`（HRESULT 均 `0x00000000`）→ `EnumPropsExW` 前后对比：

```
[baseline] hwnd=... props=1
  prop 'SysSetRedraw' handle=0x0
[after NORMAL 37/100] props=1      ← 没有任何新属性
[after TBPF_PAUSED] props=1
[after TBPF_ERROR] props=1
[after TBPF_INDETERMINATE] props=1
[after TBPF_NOPROGRESS] props=1
```

调用成功但状态不落在窗口属性表上——跨进程 `EnumProps` 读不到。**ADR-0024 的首选读取路径实测死亡。**

## Q2 · vtable hook：**不做**

ADR-0024 明文：与 ADR-0022 的禁止项同类（目标进程内存改写）。未探针。

## Q3 · MSAA 白名单轮询：**成立**（`watch_run3.txt`）

`spike.exe watch` 轮询全系统窗口 + `drive_copy.ps1`（Shell.Application CopyHere，即 explorer 自己的复制引擎）：

- 复制对话框类名 **`OperationStatusWindow`**（一个词；标题为**空**——按标题匹配永远打不中，实测 `watch_run2.txt` 先栽在这）。
- 对话框挂在**每个复制会话新起的 pid**（COM surrogate），不是 explorer.exe 自己。
- 顶层窗口的 `OBJID_CLIENT` 拿不到任何东西；必须递归遍历**子 HWND 树**再 `AccessibleObjectFromWindow(OBJID_WINDOW)` + `IAccessible` 递归。
- 读数形态：`ROLE_SYSTEM_PROGRESSBAR` 的 `get_accValue()` = `L"56%"`（`watch_run3.txt`）。

## 载荷坑（`watch_random.txt` + 探针失败两轮）

- `fsutil file createnew` 的全零文件走 NTFS 快速路径，"复制"亚秒完成，对话框存活 <100ms，任何轮询器都只能看到尾巴。
- 单个 2GB 随机文件在本机 NVMe 上 C:→D: 也只要 ~1.5s。
- **有效载荷**：几百个小随机文件（512×8MB 目录复制），对话框存活 5-8s，够 250ms 轮询取多个样本。

## 产品化映射（src/shell/taskbar_progress.*）

- 白名单 `{L"OperationStatusWindow" → AUMID L"Microsoft.Windows.Explorer"}`（explorer tile 是 ShellItem、target_path 为空，exe 名匹配不上——探针 bring-up 期间用 pin-kept 日志实测）。
- 读数 `"56%"` → 0..1 fraction；无值 → Indeterminate。
- 每轮扫描重建窗口集：销毁/离开扫描即清状态（与 `DwmUnregisterThumbnail` 同侧的生命周期纪律）。

## 附带发现

- `HKCU\...\Explorer\AccentColor` 在默认配置机器上**不存在**；真值在 `HKCU\...\Explorer\Accent\AccentColorMenu`（实测 `0xFFB16300` = Windows 默认蓝 #0063B1）。T-065 的 badge accent 读取自那时起一直走 fallback——T-067 bring-up 时发现并修复（`BadgeAccentColor` 两级读取）。

## 构建

```
g++ -std=c++20 -O2 -Wall -municode -o spike.exe spike.cpp \
    -lole32 -loleaut32 -loleacc -lshell32 -luser32 -lshlwapi -luuid
```
