# T-003 · core: 事件总线 + 应用主循环骨架 — 验收证据

- 任务: T-003
- 状态: DONE
- 时间: 2026-10-23
- 提交: 见 `git log --oneline -1`

## DoD 逐条核对

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 类型安全事件总线（subscribe/publish，同步分发，无动态分配热点） | PASS | `src/core/event_bus.h`；`tests/core_loop_test.cpp` 8 用例 |
| 2 | 主循环：消息泵 + 16ms 定时唤醒（可配置） | PASS | `src/core/app_loop.{h,cpp}`；`SetTargetIntervalMs` 钳制 1..1000 |
| 3 | 帧时间采样器输出 p50/p99 | PASS | `bench.txt` |
| 4 | 单测覆盖：多订阅者、退订、重入保护 | PASS | `test.txt`，27 用例 / 137 断言 |
| 5 | 空闲 CPU < 0.5%（60s 采样） | PASS | `idle.txt`：0.1823%（30s 采样 0.0521%） |

## 帧节奏实测（bench.txt）

修复前用 `MsgWaitForMultipleObjectsEx` 的 timeout 做帧定时，受系统定时器粒度限制，实测 `p50=30.98ms / fps=35.2`。
改用 `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` 后 `p50=16.00ms / p99=16.66ms / fps=62.5`，达到 60 FPS 硬指标。

## 空闲开销（idle.txt）

```
idle window: 60s
cpu seconds used : 0.109s
cpu percent      : 0.1823%
rss start/end    : 11.55MB / 11.47MB
handles start/end: 142 / 142
threads          : 1
```

## 已知限制

- `idle_this_frame_` 目前把「帧定时器到期」也算作 idle，语义上偏宽松；动画阶段（T-010）会改为只有 `WAIT_OBJECT_0 + nCount`（即仅消息）才算非空闲。
- 帧定时器在 `Run()` 首次调用时创建，`Run()` 不可重入；当前架构只调用一次，无需处理。
