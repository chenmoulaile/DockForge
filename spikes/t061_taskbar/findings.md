# T-061 spike · Shell_TrayWnd 显隐行为实测

日期：2026-10-25　环境：Windows 10.0.26300 x64（Win11），单显示器 1440x960@物理，
任务栏高 48px（rect 0,912,1440,960），进程非提升。spike 程序：`spike.cpp`（本目录）。

```
$ g++ -std=c++20 -O2 -Wall -o spike.exe spike.cpp -luser32
$ ./spike.exe            # Q1-Q5
$ ./spike.exe --restart  # Q6，会 taskkill explorer.exe 并重启 shell
```

## 结论（先说）

| # | 问题 | 实测结论 | 证据（spike_run_*.txt） |
|---|---|---|---|
| Q1 | 任务栏窗口类 | Win11 26300 上仍是 `Shell_TrayWnd`（`FindWindowEx` 可命中）；本机单屏 `Shell_SecondaryTrayWnd` 数量 0 | baseline 行 hwnd=0000000000020216 |
| Q2 | 同步 `ShowWindow(SW_HIDE)` 阻塞 | 实测 3.46ms / 4.13ms（explorer 消息泵空闲时很快，但这是同步等待，忙时会放大——项目已测过最差 494.59ms） | 两次 run 的 blocking call 行 |
| Q2b | 异步 `ShowWindowAsync` 收敛 | SW_HIDE 16.24ms / SW_SHOW 15.16–16.50ms 内 `IsWindowVisible` 收敛；照抄 ADR 先例**产品代码一律用 ShowWindowAsync + 轮询收敛** | ShowWindowAsync 各行 |
| Q3 | 隐藏态是否被系统自动撤销 | 隐藏后 200ms 再读仍是 visible=0，未被 explorer 自动弹回 | after async hide + 200ms 行 |
| Q4 | **工作区是否随隐藏变化** | **不变**：SPI_GETWORKAREA 恒 0,0,1440,912（隐藏前/隐藏中/恢复后三次全等）→ Dock 位置不需要随隐藏移动，也不存在压到任务栏的路径 | while hidden / after restore 行 |
| Q5 | 恢复 | `ShowWindowAsync(SW_SHOW)` 可恢复，收敛 ~16ms | restore 行 |
| Q6 | Explorer 重启自然恢复 | taskkill 后 Shell_TrayWnd=null；`start explorer.exe` 后**新** hwnd=0000000000040852 在 0.5s 内 visible=1，本进程未发任何 ShowWindow | --restart 段 |

**产品决策**（据此落地 core::taskbar）：
1. 显隐走 `ShowWindowAsync` + `IsWindowVisible` 轮询收敛（上限 500ms）；同步 `ShowWindow` 弃用。
2. 恢复语义为**强制 SW_SHOW**（不先问当前态）——紧急路径不允许被一次迟到的可见性读数挡住。
3. 不做"explorer 重启后重新隐藏"：Q6 证明重启后任务栏自然可见，这正是紧急恢复路径，**保持它是特性不是缺陷**。
4. Dock 不需要响应隐藏事件挪位（Q4 工作区不变），但仍按 C-20 发布 `TaskbarVisibilityChanged` 并重读工作区，防未来 OS 行为变化。

## Win10 与 Win11 的行为差异（DoD 条款）

- **Win11 build 26300（本机）**：以上全部实测。
- **Win10**：**本机无 Win10 环境，未运行（NOT RUN）**，不做任何"应该一致"的断言。已确认的事实
  只有两点：(a) `Shell_TrayWnd` 类名是 Win10/Win11 通用的公开 shell 窗口类（Win10 起就有，
  微软文档与公共工具普遍按它定位任务栏）；(b) 仓库目标 OS 含 "Windows 10 21H2+"，COMPATIBILITY.md
  对 Win10 两行本就标注"待测"。T-061 的 Win10 实测缺口记入 evidence/T-061/verify.txt 的
  "未运行"一节，不勾掉任何依赖它的 DoD 项。

## 已知未测边界（诚实清单）

- 多显示器 `Shell_SecondaryTrayWnd`：本机单屏数量 0，代码按枚举处理（隐藏/恢复一并覆盖），
  行为未实测。
- 任务栏"自动隐藏"模式与 SW_SHOW 的交互：本机任务栏未开自动隐藏，未测。
- 隐藏态下按 Win 键 / 打开开始菜单是否会把任务栏弹回：未测（需要真实键击，不在探针能力内）。
- spike 期间 `ShowWindow(SW_HIDE)` 同步调用只测了 explorer 空闲态两次（3.46/4.13ms），
  忙时放大数据沿用项目已知 gotcha 的 494.59ms，不在本次重测。
