# spike T-013 — 拖拽期间鼠标捕获与窗口 region

Dock 是 region 裁剪的窗口（C-12），内部重排需要指针离开图标行后窗口仍收到 `WM_MOUSEMOVE`。三件事只有实测能分开：

- **Q1** `SetCapture` 后指针离开窗口矩形，还能收到 `WM_MOUSEMOVE` 吗？
- **Q2** 窗口 region 会连捕获的鼠标消息一起裁掉吗？
- **Q3** 拖拽期间会收到 `WM_MOUSELEAVE` 吗？如果会，一个朴素的悬停处理器会在指针离开板的瞬间清掉拖拽状态。

## 四组配置

`spike_t013_drag.exe` 建一个 600x200 的 `WS_POPUP` 窗口，用 `SendInput` 驱动真实指针：按下 → 移到窗口上方 region 外 → 移到窗口外上方 → 移出右侧 → 释放。

| 组 | 命令 | 说明 |
|---|---|---|
| A | `--mode=capture --auto-drag` | 捕获，无 region |
| B | `--mode=capture --auto-drag --region` | 捕获 + 只保留下半区的 region |
| C | `--mode=capture --auto-drag --no-capture` | 不捕获，但按钮按下 |
| D | `--mode=leave` | 不按键，移入再移出（leave 的对照组） |

## 结果

| 组 | moves_inside | moves_outside | moves_captured_outside | leaves | track_arms |
|---|---|---|---|---|---|
| A | 2 | 3 | **3** | 0 | 1 |
| B | 2 | 3 | **3** | 0 | 1 |
| C | 2 | **0** | 0 | 0 | 1 |
| D | 1 | 0 | 0 | **1** | 1 |

## 结论

**Q1 — 捕获后指针离开窗口仍收到移动消息。** A 组 `moves_captured_outside=3`。C 组（不捕获、按钮按下）`moves_outside=0`：窗口在指针离开后一条消息都收不到。这组对照让 Q1 有意义——不是"窗口总能收到"，而是捕获带来的。

**Q2 — region 不影响捕获的消息投递。** B 组与 A 组逐项相同。region 是命中测试形状，捕获绕过命中测试。注意 region 仍会裁剪合成内容（spike T-010 结论 3），所以拖拽期间必须把 region 扩到整个窗口，否则抬起的图标 ghost 会被裁掉——这是绘制问题，不是消息问题。

**Q3 — 拖拽期间不会收到 `WM_MOUSELEAVE`。** A/B/C 三组都是 `leaves=0`，D 组（不按键）是 `leaves=1`。

Q3 的对照最初写错了：第一版用 C 组当对照，得出"leaves=0 说明捕获抑制了 leave"。但 C 组也有按钮按下，所以它无法区分"捕获抑制"和"按键抑制"。真正的对照是 D 组——不按键、只移动，leave 正常到达，证明这个 spike 能看见 leave。

修正后的诚实结论是：**按键按下本身就抑制 `WM_MOUSELEAVE`，与是否捕获无关**。A/B/C 全为 0 而 D 为 1，这个模式指向按键，不指向捕获。

对实现而言两者等价：拖拽期间不会被打断。但不能写成"捕获抑制 leave"，那是没有证据支持的归因。

## 第一次测量错在哪

`TrackMouseEvent` 的返回值最初被忽略。控制组报 `leaves=0` 时，唯一诚实的读法是"这个 spike 根本看不见 leave"，于是 A 组的 `leaves=0` 什么都证明不了。

修法：记录 `TrackMouseEvent` 的成功次数（`track_arms` / `track_failures`），并加 D 组作为最小对照。现在 `track_arms=1 track_failures=0` 证明请求发出去了，D 组证明消息能到达。

**没有对照组的"零"不是证据。**

## 对实现的约束

1. 拖拽用 `SetCapture`，释放时 `ReleaseCapture`。A 组 `releases=1` 说明在窗口外释放仍会收到 `WM_LBUTTONUP`。
2. 拖拽期间 region 必须覆盖整个窗口（ghost 会升到板上方，也可能横向溢出）。
3. 悬停处理器不需要为"拖拽中被 leave 打断"做防御——但需要处理 `WM_CANCELMODE`（捕获被系统抢走，例如前台切换、模态循环），把它当作"放弃拖拽"。
4. 拖拽期间不能依赖 `WM_MOUSEMOVE` 的频率做状态推进，仍要走 tick 合并（C-12）。