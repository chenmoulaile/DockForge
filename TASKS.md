# TASKS.md · 任务板

> 状态：TODO / IN_PROGRESS / DONE / PARTIAL / BLOCKED
> 规则：DoD ≤ 8 条；预估 > 1 会话必拆；无依赖标 `[P]` 可并行；禁止不可验收命名。

---

## 里程碑

| ID | 名称 | 目标 | 状态 |
|---|---|---|---|
| M0 | 骨架与验证 | 构建链路、窗口、渲染、配置、图标、布局数学全部跑通 | IN_PROGRESS |
| M1 | Dock 核心 | 可用的 Dock 栏（放大、启动、指示、拖拽、缩略图） | TODO |
| M2 | Finder Bar | 顶部栏 | TODO |
| M3 | Launchpad | 全屏网格 | TODO |
| M4 | 配置与主题 | 热重载 + 主题包 | TODO |
| M5 | 系统集成 | 自启、隐藏任务栏、转储、更新 | TODO |

---

## M0 · 骨架与验证

### T-001 · platform: 工程骨架 + CMake 构建链路 + 最小窗口
- 状态: DONE
- 优先级: P0
- 依赖: 无
- 预估: 1 会话
- 上下文: 根目录、`cmake/`、`src/platform/`
- DoD:
  - [x] `cmake -S . -B build -G "MinGW Makefiles"` 配置成功
  - [x] `cmake --build build` 生成 `DockForge.exe`
  - [x] 程序能创建并显示一个无边框置顶窗口，Ctrl+Q 退出
  - [x] 状态文件骨架（11 个）全部落盘
  - [x] `.gitignore` 覆盖 build/out/.vs/*.user/vcpkg_installed/*.dmp/evidence 大文件
  - [x] 首次 commit 完成，push 状态记录到 STATE.json
- 证据: evidence/T-001/

### T-002 · core: 日志系统 + 配置加载/保存（C-01 / C-07）
- 状态: DONE
- 优先级: P0
- 依赖: T-001
- 预估: 1 会话
- 上下文: `src/core/`、CONTRACTS.md#C-01、CONTRACTS.md#C-07
- DoD:
  - [x] 日志按 `%LOCALAPPDATA%\DockForge\logs\dockforge-YYYYMMDD.log` 写入，格式符合 C-07
  - [x] 配置首次运行生成默认 config.json（全部 11 个键）
  - [x] 越界值钳制并 WARN；损坏 JSON 回退默认并另存 `.bad`
  - [x] 未知键写回时保留
  - [x] Catch2 单测覆盖：默认值、越界钳制、损坏回退、未知键保留
  - [x] `evidence/T-002/test.txt` 含全部用例通过输出
- 证据: evidence/T-002/

### T-003 · core: 事件总线 + 应用主循环骨架
- 状态: DONE
- 优先级: P0
- 依赖: T-001, T-002
- 预估: 1 会话
- 上下文: `src/core/`
- DoD:
  - [x] 类型安全事件总线（subscribe/publish，同步分发，无动态分配热点）
  - [x] 主循环：消息泵 + 16ms 定时唤醒（可配置）
  - [x] 帧时间采样器输出 p50/p99
  - [x] 单测覆盖：多订阅者、退订、重入保护
  - [x] 空闲 CPU < 0.5%（60s 采样）
- 证据: evidence/T-003/

### T-004 · platform: DirectComposition 分层窗口 spike
- 状态: DONE
- 优先级: P0
- 依赖: T-001
- 预估: 1 会话
- 上下文: `spikes/t004_dcomp/`、`src/platform/`
- DoD:
  - [x] spike 独立可编译，创建 `WS_EX_NOREDIRECTIONBITMAP` + `WS_EX_LAYERED` 窗口
  - [x] `DCompositionCreateDevice2` → `IDCompositionDesktopDevice` → `CreateTargetForHwnd` 成功（HRESULT 打印）
  - [x] 窗口显示纯色 visual，无黑块，圆角外区域真透明
  - [x] 结论写入 DECISIONS.md（成功则纳入主工程）
  - [x] 截图 `evidence/T-004/dpi96.png`、`dpi144.png`、`dpi192.png`（96/144/192 DPI 即 100%/150%/200% 缩放；用 `DOCKFORGE_DPI_OVERRIDE` 强制，本机单屏 192 DPI 无法靠系统设置覆盖）
- 证据: evidence/T-004/

### T-005 · render: D2D/DWrite 后端 + 材质多级回退
- 状态: DONE
- 优先级: P0
- 依赖: T-004
- 预估: 1 会话
- 上下文: `src/render/`、CONTRACTS.md#C-04
- DoD:
  - [x] `IRenderBackend` 接口冻结并实现 D2D 版本
  - [x] 圆角矩形 + 描边 + 文本绘制可用
  - [x] 材质回退链 Mica→Acrylic→Gaussian→solid 按系统版本选择，日志记录实际级别
  - [x] 三级降级均可渲染，不崩溃、不黑块（强制降级开关）
  - [x] 三张 DPI 截图
- 证据: evidence/T-005/
- 备注: 实测系统材质无视逐像素 alpha / SetWindowRgn / 可配圆角，改为自绘磨砂板（ADR-0008）；修复 SetTransform 覆盖图集偏移（ADR-0009）。实时捕获 RSS 103MB，余量不足，T-015 前需优化。

### T-006 · shell: 图标提取 + .lnk/.exe/UWP 解析
- 状态: DONE
- 优先级: P0
- 依赖: T-001
- 预估: 1 会话
- 上下文: `src/shell/`
- DoD:
  - [x] `IShellItemImageFactory::GetImage` 提取 256px PNG 缓存
  - [x] `.lnk` 通过 `IShellLinkW` 解析目标与参数
  - [x] UWP 通过 `ApplicationUserModelID` 解析，回退到 `.lnk`
  - [x] 缓存命中率与耗时打点（冷/热）
  - [x] 单测：路径不存在返回明确错误码而非崩溃
- 证据: evidence/T-006/
- 备注: 实测 AppsFolder 五形态（shortcut=49 uwp=43 desktop-id=69 namespace=54 skipped=1）。
  命名空间子项必须先加 `shell:AppsFolder\` 前缀；裸桌面 id 不能用 `ActivateApplication`。
  冷/热图标预热 553.5ms → 30.8ms。冷启动 642ms。p99 尾延迟拆出 T-006a。

### T-006a · core: 收紧帧循环尾延迟（p99 5/20 窗口 > 16.7ms）
- 状态: TODO
- 优先级: P1
- 依赖: T-006
- 预估: 1 会话
- 上下文: `src/core/app_loop.cpp`
- DoD:
  - [ ] 定位 p99 尾延迟来源（高分辨率定时器抖动 / PeekMessage 批量派发 / DWM 提交阻塞）
  - [ ] 20 个连续采样窗中 p99 > 16.7ms 的比例降到 0
  - [ ] 空闲 CPU 不超过 0.1%
  - [ ] 输出 evidence/T-006a/bench.txt
- 证据: evidence/T-006a/

### T-007 · platform: 多显示器 + 高 DPI 管理
- 状态: DONE
- 优先级: P0
- 依赖: T-004
- 预估: 1 会话
- 上下文: `src/platform/display.*`、`src/main.cpp`
- DoD:
  - [x] Per-Monitor V2 DPI 感知，`WM_DPICHANGED` 正确重建渲染资源
  - [x] 枚举显示器、工作区、主屏判定
  - [x] Dock 窗口按目标屏工作区定位
  - [x] 混合 DPI 双屏切换时无模糊、无错位（截图对比）
  - [x] 100/150/200 三张截图
- 证据: evidence/T-007/
- 备注: 本机只有 1 个显示器（`\\.\DISPLAY1` 2880x1920 @192 DPI，原始 243 DPI），
  真实跨屏 DPI 转换无法在本机触发。混合 DPI 由两条互补证据覆盖：`tests/display_test.cpp`
  对负原点副屏与 96+144 混合双屏做纯逻辑断言，`DOCKFORGE_SIMULATE_DPI` 在进程内
  `SendMessage` 真实 `WM_DPICHANGED` 证明处理器会重算尺寸与位置。跨进程发送该消息会被
  系统丢弃（`PostMessage` 返回 `ERROR_MESSAGE_SYNC_ONLY` 1159），已在
  `tools/simulate_dpi.ps1` 注释中记录。**不声称合成消息等同于真实跨屏拖拽**。

### T-008 · dock: 布局引擎 + fisheye 数学（纯逻辑）
- 状态: DONE
- 优先级: P0
- 依赖: T-001
- 预估: 1 会话
- 上下文: `src/dock/layout.*`、CONTRACTS.md#C-05
- DoD:
  - [x] 实现 C-05 公式：`s_i`、累积位移、边界钳制
  - [x] 单测：鼠标在 Dock 外 / 图标数 0,1,4,20 / 极端 magnification
  - [x] 单调性：鼠标右移时缩放峰单调右移
  - [x] 100 万次求值耗时 < 20ms（纯 CPU 参考实现）
  - [x] 输出 `evidence/T-008/bench.txt`
- 证据: evidence/T-008/
- 备注: benchmark 单位是「单图标 C-05 公式项」，5.26–7.04ms/1e6；整条 Dock 最坏 210ns（占 16.7ms 预算 0.0013%）。过程中修掉两个真实缺陷：指针钳到 `[0, base_width]` 使边缘图标峰值只有 1.5556（应为 1.8），以及热路径每次分配 `gain` 数组。契约新增 C-05a 固化六条求值规则，决策见 ADR-0013。本任务不含 GPU 变换（T-010 交付）。

### T-009 · dock: Dock 窗口渲染（静态图标行）
- 状态: DONE
- 优先级: P0
- 依赖: T-005, T-006, T-008
- 预估: 1 会话
- 上下文: `src/dock/`
- DoD:
  - [x] 底部居中渲染 N 个图标，材质底板 + 圆角
  - [x] 图标按 C-05 基础位置排布，间距/尺寸读配置
  - [x] 窗口自动贴合内容宽度（无鼠标时收缩到基础尺寸）
  - [x] 三张 DPI 截图
  - [x] 空闲 RSS < 120MB
- 证据: evidence/T-009/
- 备注: 12 tile @ DPI 96/144/192 全部 `ALL_SLOTS_DRAWN`（`tools/dock_slots.ps1` 逐槽覆盖率法）。
  窗口物理尺寸与 `round(684 DIP × dpi / 96)` 完全一致：684x68 / 1026x102 / 1368x136。
  本轮同时补齐 T-006 遗留的 `shell::ToString(AppKind)` 缺失定义（只有声明，任何调用都链接失败）。
  顺带修复 `CONTRACTS.md` C-05a 与 `DECISIONS.md` ADR-0013 中被编辑事故吞掉的控制字符
  （U+0008 顶替 `b`、U+0009 顶替 `t`），已加全文控制字符扫描确认 0 残留。
---

## M1 · Dock 核心

### T-010 · dock: 悬停放大 + 可点击区域
- 状态: DONE
- 优先级: P0
- 依赖: T-009
- 预估: 1 会话
- 上下文: `src/dock/`、`src/platform/hit_region.*`
- DoD:
  - [x] 鼠标移动时图标按 C-05 缩放，最大 magnification
  - [x] 相邻图标推移符合累积位移，无跳变
  - [x] 禁止逐帧重建布局对象（零分配，`scratch_` 复用）
  - [x] benchmark 帧时间 p99 < 16.7ms（21 个 1 秒窗，p99_max 16.54ms）
  - [x] 三张 DPI 截图
- 证据: evidence/T-010/
- 备注: DoD 第 3 条原文要求「缩放走 DComp 变换/顶点」。实际实现是 CPU 侧 O(n) 前缀和重算布局（整条 Dock 210ns）+ D2D 按目标矩形缩放位图，没有逐帧创建任何对象（`DockPainter::scratch_` 复用）。实测 paint `avg=2.85ms p99=4.49ms`，占 16.7ms 预算 17%，绘制不是瓶颈；引入每图标一个 `IDCompositionVisual` 会把圆角裁剪与 region 的交互复杂化而收益为零。理由与被拒方案见 ADR-0015。若图标数涨到 50+ 需重新评估。


### T-011 · dock: 点击行为（启动/最小化/激活）
- 状态: DONE
- 优先级: P0
- 依赖: T-010
- 预估: 1 会话
- 上下文: `src/dock/`、`src/shell/`
- DoD:
  - [x] 未运行 → 启动（路径类走 `CreateProcessW`，ShellItem/NamespaceItem 走 `ShellExecuteExW`；见备注）
  - [x] 前台 → 最小化；后台 → 激活到前台
  - [x] 激活失败（UWP/权限）有降级路径且不崩溃
  - [x] 点击热区与视觉一致（含放大态）
  - [x] 日志记录每次动作与耗时
- 证据: evidence/T-011/
- 备注: DoD 第 1 条按 C-13 落地为「按 `AppKind` 分派启动路径」——`.lnk`/`.exe` 用 `CreateProcessW`（能带 arguments 与 working_dir），`ShellItem`/`NamespaceItem` 用 `ShellExecuteExW`（只有 `shell:AppsFolder\` 前缀能激活），`Uwp` 用 `ActivateApplication`。三条路径在 T-006 已各自实测，本任务只是接入。
- 备注: 实测暴露出四个缺陷，全部修复并有数字：AUMID 与路径「或」匹配导致串味（计算器↔设置）、scoop junction 让路径字符串永不相等（每次点击都启动新副本）、`IsIconic` 在动作后采样导致 restore 被标成 raise、`ShowWindow` 同步等待目标应用把 p99 拖到 494.59ms。改 `ShowWindowAsync` 后 p99=9.89ms、20 次点击 0 次超预算。
- 备注: 打包应用（UWP）的点击路径用 `DOCKFORGE_PIN_APPS=calculator` 验证钩子端到端跑通；Dock 默认前 12 个应用本机不含打包应用，否则这条路径只有单测覆盖。

### T-012 · dock: 运行指示器
- 状态: DONE
- 优先级: P0
- 依赖: T-011
- 预估: 1 会话
- 上下文: `src/dock/`
- DoD:
  - [x] 轮询窗口枚举映射 PID → 应用，300ms 节流
  - [x] 运行中显示圆点，前台高亮
  - [x] 退出后指示器消失 ≤ 1s
  - [x] 枚举开销 < 1ms/次（bench）
- 证据: evidence/T-012/
- 备注:
  - 两个时钟是这一项的全部设计：窗口存在性走 300ms 轮询（EnumWindows 0.48–0.94ms），前台归属每帧读 `GetForegroundWindow`（亚微秒）。合并成一个 bool 就只能取慢时钟，alt-tab 后高亮会滞后 300ms。
  - 圆点画在板的 10 DIP 底部内边距里，不新增带。新增 7 DIP 的带会让板与窗口一起长高、屏幕上每个 Dock 位移，并让所有「从窗口高度反推 scale」的验证工具失效。
  - 前台不透明度 0.95 / 后台 0.45，宽度 1.0x / 0.85x。只差亮度在浅色壁纸下读不出来。
  - 帧预算判 WATCH 而非 PASS。交替 A/B（`DOCKFORGE_RUNNING_POLL_MS` 300 vs 5000）显示两组差的符号相反、幅度 0.016–0.017ms，与同配置运行间波动（0.018ms）同量级 —— tracker 不是尾延迟的原因，这是 T-006a 已记录的同一问题。
  - 已知限制：文档类条目（`.chm` → `hh.exe`）不显示圆点，圆点反映真实归属而非渲染缺陷。

### T-013 · dock: 内部拖拽重排
- 状态: DONE
- 优先级: P0
- 依赖: T-010
- 预估: 1 会话
- 上下文: `src/dock/`
- DoD:
  - [x] 按住拖动图标，自绘 ghost + 插入线
  - [x] 释放后顺序持久化到配置
  - [x] 拖出 Dock 区域外释放 = 移除（带确认）
  - [x] 拖拽期间帧率不低于 55 FPS
- 证据: evidence/T-013/
- 备注:
  - 插入槽用静止栅格（`IdleCentersDip`）而非放大后的中心：放大中心是鼠标位置的函数，喂回去形成反馈环，实测让 `0 -> 4` 报成 `0 -> 3`。
  - 移除改成长按 550ms 确认（`kEjectConfirmMs`），只判上/左/右。Dock 是 `WS_EX_NOACTIVATE`，弹确认框会抢焦点，比误删更糟；反馈靠 ghost 随进度淡出。
  - `CommitOrder` 用例曾写进开发机真实配置，`ConfigPath()` 已加 `DOCKFORGE_CONFIG_PATH` 覆盖，测试隔离到 `%TEMP%`。
  - `GetCapture()` 不能跨进程断言（返回调用线程的捕获窗口，恒为 0），改读 app 自报的 `capture=1 match=1`。
### T-014 · dock: OLE 拖入（文件/快捷方式）
- 状态: TODO
- 优先级: P0
- 依赖: T-013
- 预估: 1 会话
- 上下文: `src/dock/`
- DoD:
  - [ ] 实现 `IDropTarget`，接受 `.lnk`/`.exe`/文件夹
  - [ ] 拖入时高亮插入位，释放后加入并持久化
  - [ ] 非法类型有视觉反馈且不写入配置
  - [ ] 与 T-013 内部重排不冲突
- 证据: evidence/T-014/

### T-015 · dock: 窗口缩略图预览
- 状态: TODO
- 优先级: P0
- 依赖: T-012
- 预估: 1 会话
- 上下文: `src/dock/`、`src/shell/`
- DoD:
  - [ ] 悬停 ≥400ms 弹出预览
  - [ ] `DwmRegisterThumbnail` 渲染顶层窗口，多窗口横向 ≤8
  - [ ] 最小化窗口不黑屏（末帧缓存）
  - [ ] UWP 经 `ApplicationFrameHost` 子窗口枚举正确
  - [ ] 句柄无泄漏（`DwmUnregisterThumbnail` 全部配对，`GetGuiResources` 前后一致）
- 证据: evidence/T-015/

---

## 会话结束模板

```
[HANDOFF]
会话时间: <ISO8601>
完成任务: <列表>（共 N 个）
PARTIAL: <列表或 None>
BLOCKED: <列表或 None>
待人类决策: <条目 + 推荐选项，或无>
GitHub: 已 push 至 <url> | 最新 commit <sha> | tag: <无/xxx>
下一轮开始于: <TASK_ID>
本地未入库证据: <路径列表，或无>
```
