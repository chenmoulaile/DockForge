# evidence/T-001 · platform: 工程骨架 + CMake 构建链路 + 最小窗口

日期：2026-10-23
工具链：g++ (MSYS2 mingw-w64) 15.2.0 / CMake 4.4.2 / MinGW Makefiles

## DoD 核对

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | `cmake -S . -B build -G "MinGW Makefiles"` 配置成功 | PASS | `build.log` 前段 `Configuring done / Generating done` |
| 2 | `cmake --build build` 生成 `DockForge.exe` | PASS | `build.log` 末段 `Built target DockForge`；产物 `build/bin/DockForge.exe` |
| 3 | 无边框置顶窗口显示，Ctrl+Q 退出 | PASS | `win.png` 720x120 截图，中心像素 `24,26,32` 为自绘底色，309 个浅色采样点为文字像素；`WM_KEYDOWN`+`GetKeyState(VK_CONTROL)` 处理见 `src/main.cpp` |
| 4 | 11 个状态文件骨架落盘 | PASS | `PROMPT/STATE/PROGRESS/TASKS/CONTRACTS/DECISIONS/DECISIONS_PENDING/BLOCKERS/ASSETS/PERFORMANCE/COMPATIBILITY` 均在仓库根 |
| 5 | `.gitignore` 覆盖构建与临时产物 | PASS | `.gitignore` 含 `build/ out/ .vs/ *.user vcpkg_installed/ *.dmp evidence/**/*.mp4` |
| 6 | 首次 commit 完成，push 状态入 STATE.json | PASS(commit) / SKIPPED(push) | 见 `git log`；`STATE.json.remote.status = "unconfigured"`，原因见 `BLOCKERS.md` B-001 |

## 构建命令（可复现）

```
& .\tools\build.ps1 -Action all
```
等价于：
```
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-mingw.cmake \
  -DCMAKE_MAKE_PROGRAM=D:/MSYS2/mingw64/bin/mingw32-make.exe \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j 8
build/bin/dockforge_tests.exe
```

## 运行证据

日志（`%LOCALAPPDATA%\DockForge\logs\dockforge-20261023.log`）：
```
2026-10-23T23:15:33.872+08:00 INFO [app] logger started, file=...
2026-10-23T23:15:33.872+08:00 INFO [app] DockForge 0.1.0 starting
2026-10-23T23:15:33.872+08:00 INFO [app] config path = C:\Users\chenk\AppData\Roaming\DockForge\config.json
2026-10-23T23:15:33.873+08:00 INFO [platform] window created hwnd=... dpi=192
```
日志格式符合 `CONTRACTS.md` C-07（ISO8601 带毫秒与偏移、LEVEL、`[module]`）。

## 过程中修复的真实缺陷（已复现并验证）

1. `WS_EX_NOREDIRECTIONBITMAP` + GDI `FillRect` → 截图全黑。根因：该扩展样式去掉 DWM 重定向表面，GDI 绘制不可见。修复：`WindowDesc.composition_surface` 改为显式开关，骨架阶段关闭，T-004 起由 DComp 接管。
2. `Logger::Init` 在持有 `mutex_` 时调用 `Log()`（内部再次加锁）→ 进程启动即死锁，日志文件 0 字节。修复：横幅日志移出锁作用域。
3. `_wfopen_s` 独占写句柄导致外部无法读取日志。修复：`_wfsopen(..., _SH_DENYWR)`。
4. 全局 `UNICODE` 使 Catch2 amalgamated 生成 `wmain`，console 子系统链接失败。修复：测试目标加 `DO_NOT_USE_WMAIN`。
5. `wWinMain` 需 `-municode` 才能链接（此前 `undefined reference to WinMain`）。
6. `FOLDERID_*` 需 `-luuid`（`KNOWN_FOLDER` GUID 定义）。

## 遗留

- GitHub remote 未配置（`gh` 未登录），push 全部 SKIPPED，见 `BLOCKERS.md` B-001。
- 骨架窗口为 GDI 占位绘制，T-004/T-005 替换为 DirectComposition + Direct2D。
