# T-062 spike · MiniDumpWriteDump under MinGW-w64（实测结论）

日期：2026-10-25。环境：Windows 10.0.26300 x64，g++ (MSYS2 mingw-w64) 15.2.0，
dbghelp 头 `D:/MSYS2/mingw64/include/psdk_inc/_dbg_common.h:1676`（经 `dbghelp.h` 引入），
import lib `D:/MSYS2/mingw64/lib/libdbghelp.a`。

## 构建与运行

```
$ cd spikes/t062_dump
$ g++ -std=c++20 -O2 -Wall -o spike.exe spike.cpp -ldbghelp      # 退出 0
$ ./spike.exe > spike_run.txt 2>&1                                # 故意崩溃
$ cat spike_run.txt
[spike] pid=3484
[spike] previous filter=00007ff6ec6f1800 (TaskbarStyleFilter=00007ff6ec6f1800) chain_installed=1
[spike] arming null dereference
head filter: dumped=1 err=0 ms=62 file=spike-crash-3484.dmp
exit_code=139                 # bash 对 0xC0000005 的 128+11 映射
wall_ms=1741                  # 含 300ms 预备 Sleep + 进程启动/终止开销
$ cat spike-marker.txt
head filter: dumped=1 err=0 ms=62 file=spike-crash-3484.dmp
tail filter ran: taskbar-restore step executed
$ file spike-crash-3484.dmp
spike-crash-3484.dmp: Mini DuMP crash report, 13 streams, ..., 0x200000 type
```

## 逐问题结论

| # | 问题 | 实测结论 |
|---|---|---|
| Q1 | MinGW 能否链接 `MiniDumpWriteDump`？ | **能**。`-ldbghelp` 链接通过，`MINIDUMP_TYPE` / `MINIDUMP_EXCEPTION_INFORMATION` 编译干净（g++ -Wall 零警告）。工具链提示属实。 |
| Q2 | 过滤器内写 dump 是否成功、耗时？ | **成功，62ms**（`dumped=1 err=0`，QPC 计时）。产品 DoD 预算 2 秒余量 32 倍。dump 被 `file` 识别为合法 crash report（13 streams）。 |
| Q3 | 过滤器返回后进程是否立即退出？ | **是**。崩→退出全程 1.74s（含 300ms Sleep 与启动开销），无 WER 弹窗挂起（`SEM_NOGPFAULTERRORBOX` 已在安装时设置）。退出码 0xC0000005。 |
| Q4 | 双过滤器链是否工作？ | **工作**。`SetUnhandledExceptionFilter` 的返回值就是先装的 taskbar 风格过滤器；崩溃时 head（dump）先跑，随后 marker 证明 tail（恢复）也跑了。注意：**prev 过滤器只能在安装时保存**——Win32 没有 API 读回当前过滤器，产品实现必须把 dump 过滤器装在 T-061 之后并把返回值存下来转发。 |
| Q5 | 有存活工作线程时 dump 是否照常？ | **是**。spike 常驻一个 Sleep 循环线程（模拟 D2D/DComp 内部线程），dump 正常 13 streams，未卡死。 |

## 产品实现依据

1. 安装顺序：`InstallTaskbarCrashRecovery()`（T-061，main.cpp:326 已有）在前，
   `InstallCrashDumper()` 在后；后者保存前者的指针，崩溃时先写 dump 再转发。
2. dump 参数：`MiniDumpNormal` + `MINIDUMP_EXCEPTION_INFORMATION{ThreadId=GetCurrentThreadId(),
   ExceptionPointers=info, ClientPointers=FALSE}`；文件名 `dockforge-crash-YYYYMMDD-HHMMSS.dmp`
   （带时间戳防覆盖），目录 `core::DumpsDir()`（paths.cpp:63 已有）。
3. 过滤器内 raw Win32 only（CreateFileW / MiniDumpWriteDump / QPC / WriteFile），
   不碰 CRT 堆与 logger——与 T-061 过滤器同一纪律。
4. 预算与不阻塞：转储耗时用 QPC 实测并写入 `%LOCALAPPDATA%\DockForge\logs\crash-dump.txt`
   标记行（logger 可能已死），探针据此断言 < 2000ms 且进程退出。
5. 局限（如实记录）：`MiniDumpWriteDump` 无超时参数；2 秒预算靠最小 dump 类型 +
   实测背书（62ms），不是可强制的看门狗。若未来实测超预算，再评估看门狗线程方案。
