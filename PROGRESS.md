# PROGRESS.md · 执行日志（按时间正序追加）

> 每任务追加一条。格式见 PROMPT.md · CLOSE PROTOCOL。

## 2026-10-23T22:55:00+08:00 | BOOTSTRAP | DONE
- 做了什么：环境勘察（无 MSVC / 无 Windows SDK / 无 vcpkg；有 MSYS2 g++ 15.2.0 + CMake 4.4.2 + mingw32-make）；建立目录骨架；落盘 11 个状态文件；`git init` 于 `D:\MacDock`，分支 `main`，身份 `DockForge Agent <dockforge@local>`。
- 关键决策：ADR-0002（改用纯 Win32 + DComp + D2D，依据原 ADR-0001 回退条款）；ADR-0003（vendoring nlohmann/json 3.11.3 + Catch2 3.5.4）。
- 证据：无（环境勘察结论已写入 DECISIONS.md / STATE.json.toolchain）
- 遗留：gh CLI 未登录 → 无 GitHub remote，已记 BLOCKERS.md B-001；本地 commit 正常。
- 下轮开始于：T-001

## 2026-10-23T23:20:00+08:00 | T-001 | DONE
- 做了什么：CMake + MinGW 构建链路跑通（`tools/build.ps1 -Action all`）；无边框置顶窗口 + Ctrl+Q 退出；core/paths + core/log 基础层落地；Catch2 测试目标接入（3 用例 8 断言全通过）。
- 证据：evidence/T-001/{build.log,test.txt,verify.md,win.png}
- 关键缺陷（已修）：WS_EX_NOREDIRECTIONBITMAP 使 GDI 绘制不可见；Logger::Init 持锁重入死锁；_wfopen_s 独占句柄；Catch2 在 UNICODE 下生成 wmain 链接失败。
- 遗留：GitHub remote 仍未配置（gh 未登录），push SKIPPED。
- 下轮开始于：T-002

## 2026-10-23T23:30:00+08:00 | T-002 | DONE
- 做了什么：`src/core/config.{h,cpp}` 落地（C-01 全 11 键 + 钳制 + 自愈回写 + 未知键保留 + 原子写）；`src/main.cpp` 接线配置，窗口尺寸/文案随配置变化；Catch2 扩到 13 用例 92 断言。
- 证据：evidence/T-002/{build.log,test.txt,verify.md,runtime.log,default-config.json,normalized-config.json,win.png}
- 关键缺陷（已修）：浮点序列化泄漏二进制噪声（`1.8f`→`1.7999999523162842`）；MinGW `std::ifstream` 不收 `wstring` 路径（改 Win32 IO）；`ReadEnum` 用 `auto` 形参导致模板推导失败（改显式 `Enum` 模板）。
- 遗留：热重载属 M4；`behavior.*` 仅解析未生效。
- 下轮开始于：T-003
