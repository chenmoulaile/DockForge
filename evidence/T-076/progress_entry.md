## 2026-10-26T00:09:00+08:00 | T-076 | DONE
- 做了什么：macOS 风格通知中心。`tools/notification_sidecar.cs`（UserNotificationListener 的 .NET 子进程，
  poll/clear/remove/quit + push 行，与 hw_sidecar 同一套 stdin/stdout 行协议）；`src/platform/line_process.{h,cpp}`
  （从 media_sidecar.cpp 原样搬迁的子进程行管道，两处 sidecar 共用，hw_sidecar 的 14 用例 67 断言未受影响）；
  `src/notifications/`（notification_types 行协议解析 + 文本回落链、notification_store 仅内存历史 40 条上限 +
  首次 poll 只做基线 + seen 集合防重播、notification_sidecar 桥 + FoldNotificationLines 纯函数、notification_banner
  右上角堆叠窗 4 张上限 + 每卡独立超时 + 点击跳转、notification_panel 历史面板 8 行视窗 + 滚动 + 清除、
  notification_targets AUMID 严格 + 显示名宽松匹配）；`src/render/material_events.h` + `material_monitor.{h,cpp}`
  （C-21 落地：全进程唯一 GetSystemPowerStatus 轮询点，5s 一次、翻转才发事件，降级 mica→acrylic→blur→solid
  只降不升，恢复一次跳回 baseline）；`dock_view` 增 C-22 pulse（PulseAlphaFor，320ms×2 不循环，
  DockTile.pulse_start_ms）；config 增 NotificationsConfig 五键（范围 2..30 / 1..30，SameNotifications）；
  设置窗口 Behavior 增五行；main.cpp 接线（sidecar 生命周期与每 poll 间隔最多重启一次、投递函数 notif_deliver
  供真实到达与探针 push 共用、探针通道 DOCKFORGE_NOTIFICATIONS_CMD_FILE/_STATE_FILE、C-21 订阅驱动两个通知窗换 tier）；
  CMake 增 notification_sidecar 与 notification_sidecar_stub 两个目标。
- 实测：spike_async_pump.log（MinGW 直连 WinRT 在 STA + 真实消息泵 + 8s PeekMessage 下仍恒 Started，
  故必须走 .NET 子进程）；spike_notif_text.log（非打包进程 GetAccessStatus=Allowed、183ms 枚举 62 条、
  二次枚举 id 完全一致、正文路径是 Visual.Bindings[i].GetTextElements()[k].Text——文档的 Notification.Content
  在该投影不存在）；spike_sidecar_protocol.log（ready 90ms、poll 693-707ms/63 条、未知 id remove 回 ok、
  非法命令回 bad command、quit 退出 0）；build 全目标 exit 0；全量测试 4554 断言 448 用例 exit 0；
  [notifications] 32 用例 181 断言 exit 0；探针两种模式全 PASS（默认 A B C D E G H I J，F 跳过；
  -StubSidecar 十步全 PASS 含 F 清除回路），J 步实测 banner/panel 同步 blur → solid → blur。
- 过程中修掉的真 bug：① notification_types.cpp 的 Trim 谓词极性写反，遇非空格字符就删、整串被吃光；
  ② 探针命令整行被写进日志含通知标题正文（违反 ADR-0025，被探针 H 步抓到），改成只记动词，连带去掉
  arrived 日志的 title 字段；③ CMake 给 csc 传含正斜杠的路径会被当选项切开 → CS1504，既有 bug
  （hw_sidecar 在干净构建下同样失败，已复现），改 file(TO_NATIVE_PATH) + 反斜杠；④ notification_panel.cpp
  的 Relayout ↔ Refresh 无限互递归，面板可见时收到 push 就栈溢出、进程无 crash 日志地消失（已复现并修复）；
  ⑤ 探针自身两处不稳定：PowerShell 5.1 的 Set-Content -Encoding UTF8 写 BOM 让首行命令失效、固定 Start-Sleep
  抢在通知中心初始化之前，改为显式无 BOM 写入 + 轮询日志/文件时间戳。
- 诚实边界（evidence/T-076/verify.txt）：清除按钮的实机验证走 -StubSidecar 桩，真实 ClearNotifications
  未被自动化跑过（会清掉操作者机器上的真实通知），需要时加 -ClearRoundTrip；点击跳转只做了代码路径 + 单测，
  未做真人鼠标点击实机验证；面板锚点暂挂工作区右上角而非 Finder Bar 时钟（M2 仍 TODO，沿用 T-068 先例）；
  C-21 的电池省电分支未被实机触发（本机未进入省电模式），实机跑到的是设备丢失分支；
  Launchpad 与 FolderPanel 仍未订阅 C-21（既有缺口）。
- 下轮开始于：T-085 社区主题分发替代方案（P1）
