# T-012 验证报告 · dock 运行指示器

## DoD 核对

| # | 条目 | 状态 | 证据 |
|---|---|---|---|
| 1 | 轮询窗口枚举映射 PID → 应用，300ms 节流 | 通过 | `idle.txt`（60s 内 `polls` 数 = 时长/0.3）、单测 `the tracker polls on its interval, not every frame` |
| 2 | 运行中显示圆点，前台高亮 | 通过 | `indicator_states.txt`：后台 `max_delta=160.5`、前台 `max_delta=339.5`（2.1 倍） |
| 3 | 退出后指示器消失 ≤ 1s | 通过 | `indicator_states.txt`：`exit_detected=True elapsed_ms=437 budget_ms=1000` |
| 4 | 枚举开销 < 1ms/次 | 通过 | `idle.txt` 期间 `run polls=... avg=0.48-0.54ms max=0.83-0.94ms`；单测断言 `max_poll_ms < 100` 作为上界 |

## 设计：两个时钟

指示器要回答两个问题，它们的更新频率差三个数量级：

| 状态 | 来源 | 成本 | 频率 |
|---|---|---|---|
| 应用是否有窗口 | `EnumWindows` 全扫 | 0.48–0.94ms | 300ms |
| 哪个窗口在前台 | `GetForegroundWindow` | 亚微秒 | 每帧 |

用一个 bool 把两者合并，就只能取慢时钟——alt-tab 后高亮要滞后最多 300ms。所以 `TileRunState` 是三值（`Idle`/`Running`/`Foreground`），`RunningTracker::Update` 返回「可见状态是否可能变化」，调用方据此决定是否重绘。

`Update` 在窗口集合未变时返回 false。空闲桌面上每次轮询都返回 false，这是 Dock 空闲 CPU 0.0159% 而不是每 300ms 重绘一次的原因。

## 指示器几何

圆点画在板的底部内边距里，不新增一条带。

- 板的底边就是窗口的底边：画得更低会同时落在板和窗口 region 之外，`SetWindowRgn` 会裁掉它。
- 新增 7 DIP 的带会让板与窗口一起长高，屏幕上每个 Dock 都位移，并让所有「从窗口高度反推 scale」的验证工具失效。
- 内边距是 10 DIP，4 DIP 的圆点居中后距图标 3 DIP —— 与 macOS 的比例一致。

前台圆点不透明度 0.95、后台 0.45，宽度 1.0x / 0.85x。两项都不同而不是只差亮度：48 DIP 图标下 4 DIP 的点很小，只靠透明度区分在浅色壁纸上会读不出来。

## 四态实测

`tools/t012_evidence.ps1` 依次制造四种状态，每次都截图并用 `tools/indicator_probe.ps1` 采样圆点所在的像素行：

| 状态 | with_dot | brightest_delta | verdict |
|---|---|---|---|
| idle（无应用运行） | 0 | 0.0 | NO_DOTS |
| foreground（7-Zip 在前台） | 1 | **339.5** | PARTIAL |
| running（7-Zip 在后台） | 1 | **160.5** | PARTIAL |
| exited（7-Zip 已关闭） | 0 | 0.0 | NO_DOTS |

`PARTIAL` 在这里是正确的判定：12 个槽位里只有 1 个有圆点。探针的 `DOTS_PRESENT` 只在 12 个全有圆点时给出，那种情况在真实桌面上不该出现。

退出检测：`exit_detected=True elapsed_ms=437`，预算 1000ms。437ms ≈ 一次 300ms 轮询 + 一帧绘制。

## 三个工具缺陷（都修了，都影响过结论）

| 缺陷 | 症状 | 根因 |
|---|---|---|
| 探针漏加行原点 | 圆点在 x=135，探针采样 x=48，报 `NO_DOTS` | 图标行在比内容更宽的窗口里居中，原点不是 padding |
| 探针硬编码板色 | 背景换成白色窗口后 12 个槽位全报有圆点 | solid 材质仍混合 8% 背景，板色随背景从 `(13,15,18)` 变到 `(33,35,38)` |
| `click_probe.ps1` 退出即杀进程 | 截图时进程已不在 | 默认 teardown 杀了被测量的对象；加 `-KeepRunning` |

第三个缺陷最危险：它让「截不到图」看起来像「Dock 崩了」。

## 帧预算

`evidence/T-012/ab_tracker_cost.md` 记录了一次交替 A/B：同一构建，只改 `DOCKFORGE_RUNNING_POLL_MS`（300ms 生产值 vs 5000ms 等于每次运行只轮询一次）。

| run | poll | p99_mean | p99_max | over_budget |
|---|---|---|---|---|
| poll300 | 300ms | 16.572ms | 16.90ms | 2/15 |
| poll5000 | 5000ms | 16.555ms | 16.73ms | 1/15 |
| poll300b | 300ms | 16.554ms | 16.60ms | 0/15 |
| poll5000b | 5000ms | 16.570ms | 16.67ms | 0/15 |

两组差的符号相反、幅度 0.016–0.017ms，与同一配置的运行间波动（0.018ms）同量级。**tracker 不是尾延迟的原因**：每秒 3 次、每次 0.5ms 的枚举挪不动一个已经被 16ms 定时器量化决定的 p99。

判为 **WATCH** 而非 PASS，理由就是这张表：一个「开不开这个功能都一样动」的数字，不能拿来当这个功能的证据。这与 T-006 已记录的尾延迟问题（T-006a）是同一件事，不是 T-012 引入的回归。

## 空闲开销（60s）

| 指标 | 实测 | 预算 | 结论 |
|---|---|---|---|
| CPU | 0.172s / **0.0159%** | <0.5% | PASS |
| RSS | 90.46 → 90.43MB | <120MB | PASS |
| 句柄 | 372 → 362 | 无增长 | PASS |
| 线程 | 6–10 | 稳定 | PASS |

RSS 比 T-011 的 92.79MB 略降，是 tile 图标内存的自然回收，不是新功能带来的节省。

## 测试

`All tests passed (1458 assertions in 125 test cases)`，其中 `[indicator]` 标签 13 用例 / 35 断言：

- `ToString` 覆盖三值
- 轮询间隔钳制到 50..5000
- 无窗口的应用是 `Idle`
- 真实窗口的应用不是 `Idle`（不假设机器空闲，只断言关系）
- 窗口销毁后回到 `Idle`
- 同一窗口集合的轮询返回 false
- 300ms 间隔下 200ms 内不轮询、超过后轮询
- `Invalidate` 强制下一次轮询
- 轮询耗时被记录且有上界
- `ApplyToTiles` 覆盖每个 tile
- 空 tile 列表不崩
- 圆点尺寸在板内边距之内（断言关系而非注释）
- 几何暴露指示器所依赖的 baseline

## 已知限制

- **文档类条目不显示圆点**。tile 6「AutoHotkey Help File」指向 `.chm`，点击后前台是 `hh.exe`，与条目路径不同，路径匹配必然失败。圆点反映的是真实归属，不是渲染 bug。同类问题适用于任何「条目指向 A、实际运行 B」的 shell 条目。
- **窗口归属靠路径/AUMID 匹配**，同一进程开多个窗口时只高亮第一个命中的窗口。多窗口应用（浏览器多窗口）的逐窗口指示要等 T-015 缩略图提供窗口列表。
- 单显示器主机，无法验证跨屏时指示器跟随。
- 帧预算判 WATCH 而非 PASS，原因见上面的 A/B 表。
