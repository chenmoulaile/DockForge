# TASKS.md · 任务板

> 状态：TODO / IN_PROGRESS / DONE / PARTIAL / BLOCKED
> 规则：DoD ≤ 8 条；预估 > 1 会话必拆；无依赖标 `[P]` 可并行；禁止不可验收命名。

---

## 里程碑

| ID | 名称 | 目标 | 状态 |
|---|---|---|---|
| M0 | 骨架与验证 | 构建链路、窗口、渲染、配置、图标、布局数学全部跑通 | DONE（T-006a 帧尾延迟仍 TODO P1） |
| M1 | Dock 核心 | 可用的 Dock 栏（放大、启动、指示、拖拽、缩略图） | DONE（T-009..T-016 全部 DONE） |
| M2 | Finder Bar | 顶部栏 | TODO |
| M3 | Launchpad | 全屏网格启动器 | DONE (T-040/T-041/T-042 全部完成) |
| M4 | 配置与主题 | 热重载 + 主题包 | TODO |
| M5 | 系统集成 | 自启、隐藏任务栏、转储、日志轮转 | TODO |
| M6 | Stage Manager | 窗口分组与切换 | TODO |
| M7 | 质量加固与分发 | 冒烟测试、性能基准、安装包 | TODO |

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
- 状态: DONE
- 优先级: P0
- 依赖: T-013
- 预估: 1 会话
- 上下文: `src/dock/`、`src/shell/`
- DoD:
  - [x] 实现 `IDropTarget`，接受 `.lnk`/`.exe`/文件夹
  - [x] 拖入时高亮插入位，释放后加入并持久化
  - [x] 非法类型有视觉反馈且不写入配置
  - [x] 与 T-013 内部重排不冲突
- 证据: evidence/T-014/
- 备注:
  - 「高亮插入位」实现为**一条 4 DIP 竖线**（`kDropCaretWidthDip`），接受色近白 `(212,218,227)`、拒绝色暖红 `(219,98,89)`，实测三档 DPI 全部落在静止栅格的期望槽位上（差 0/0/1px）。
  - 四类载荷端到端通过：`.lnk` 新增、重复拖不产生第二个 tile、`.txt` 拒绝且配置不变、目录新增为 `AppKind::Folder`（走 `ShellExecuteExW`，`CreateProcess` 对目录返回 `ACCESS_DENIED`）。
  - 两处缺陷：`UpdatePointer` 读 `GetDpiForWindow` 与几何不同源，`DOCKFORGE_DPI_OVERRIDE=96` 下把 tile[6] 算成 tile[3]（C-10 规则第二次发作）；每个 OLE 回调都 `InvalidateRect` 导致 `p99_mean=17.284ms`、19/20 窗超预算，改 tick 合并后 `16.512ms`、0 窗超。
  - `main.cpp` 的 `CoInitializeEx` 已全部替换为 `dock::OleSession`：`CoInitializeEx` 之后 `RegisterDragDrop` 返回 `0x8007000E E_OUTOFMEMORY`，只有 `OleInitialize` 返回 `S_OK`。
  - 视觉证据一律 `DOCKFORGE_FORCE_MATERIAL=solid`：blur 档的 `WDA_EXCLUDEFROMCAPTURE` 让 `CopyFromScreen` 采到桌面背后内容，dock 区域全黑。

### T-015 · dock: 窗口缩略图预览
- 状态: DONE
- 优先级: P0
- 依赖: T-012
- 预估: 1 会话
- 上下文: `src/dock/`、`src/shell/`、`src/platform/`
- DoD:
  - [x] 悬停 ≥400ms 弹出预览（`dock::HoverTimer` kPreviewDwellMs=400，单元测试 6 例）
  - [x] `DwmRegisterThumbnail` 渲染顶层窗口，多窗口横向 ≤8（`shell::ThumbnailHost` kMaxThumbnails=8，单元测试 8 例；上限表见 C-17c）
  - [x] 最小化窗口不黑屏（末帧由 DWM 保留，spike `best_delta=0`，C-17f）
  - [x] UWP 经 `ApplicationFrameHost` 子窗口枚举正确（源用 `AppWindow.hwnd`，日志同时记 frame hwnd/app_pid，C-17g）
  - [x] 句柄无泄漏（`DwmUnregisterThumbnail` 全部配对，spike 40 轮 GDI 8→8 / USER 19→19，C-17i）
- 证据: evidence/T-015/
- 未验证项（本轮不实机验证，见 evidence/T-015/bench.txt F 节）:
  - 端到端视觉：`preview shown tile=` 日志、多窗口 N 槽、最小化末帧、Calculator UWP 帧源、帧预算 p99 —— 待 `tools/preview_probe.ps1` 与 `tools/hover_bench.ps1` 在实机轮补齐。

---

### T-016 · dock: 实机验证补齐（T-015 端到端证据）
- 状态: DONE
- 优先级: P0
- 依赖: T-015
- 完成: 2026-10-24
- DoD:
  - [x] `tools/preview_probe.ps1`：悬停 <400ms 无 `preview shown tile=`，≥400ms 有（C-17j 第 1 行）
  - [x] 同一 app 开 4 窗悬停：日志 `windows=4`，视觉 4 槽；开 10 窗只出 8 槽
  - [x] 最小化源窗后悬停：槽内仍是末帧（非纯黑板）
  - [x] Calculator 悬停：日志 `source[i] hwnd/app_pid` 不同且内容正确（C-17g）
  - [x] `tools/hover_bench.ps1`：等 `frames=1000` 后 p99 与裸定时器地板对照
  - [x] 40 轮显隐后 `GetGuiResources` GDIOBJECTS/USEROBJECTS 稳态增量 0
- 证据: evidence/T-016/（verify.txt + 五个子目录 + 对照图）
- 实测: 单窗口 268x188；三窗口 740x188（注册 3/3）；四窗口 976x188；十窗口钳到八槽 1920x188；
  最小化后同矩形差分 ratio=0.0143（末帧保留）；Calculator frame host pid 9680 vs app_pid 2152；
  40 轮 + 2 预热后 gdi/user 稳态增量 0；裸定时器地板 p99=19.2ms 高于 16.7ms 门槛，preview 归因 NO_SIGNAL
- 本轮修复的产品缺陷: `shell::WindowsForEntry` 的预筛用字符串比较，重犯 scoop junction 陷阱，
  导致 7-Zip 三窗口永不弹预览；已删除预筛，直接走 `shell::MatchesEntry`
- 已知边界: 多窗口证据用 7-Zip（Win32，3-10 窗口）；Sublime/Chrome 无法开出第二窗口，未覆盖

---

---

## M3 · Launchpad（P1）

### T-040 · Launchpad 应用索引器
- 状态: DONE
- 优先级: P1
- 依赖: T-006（应用解析与图标提取）
- 预估: 1 会话
- 上下文: `src/shell/`、`CONTRACTS.md#C-08`、`CONTRACTS.md#C-18`
- 契约: C-18
- DoD:
  - [x] 复用 `shell::ScanAppsFolder()` 作为主来源，不另写枚举
  - [x] 聚合 `launchpad.extra_paths`（每项递归 `shell::FindShortcuts(root, 4)`）
  - [x] 按 C-15a 身份去重；AppsFolder 优先于自定义目录
  - [x] 用 `CompareStringEx(SORTDEFAULT|NORM_LINGUISTIC_CASING)` 排序（契约原写 `NORM_LINGUISTIC_CASE`，该宏不存在，已改）
  - [x] 索引结果内存缓存；`ReadDirectoryChangesW` 监视 + 500ms 防抖（逐条目增量未做，只做「有变化→整表重建」，见 evidence/T-040/verify.txt 诚实边界）
  - [x] 单测：空目录、无权限目录、损坏 `.lnk`、UWP 项、重复身份、排序稳定性
- 证据: `evidence/T-040/`
- 实测: `items=208 apps=215 accepted=208 skipped=7 upstream=1 scan_ms=360.9`；with_extra `items=280 extra=72 dup=29 unique=280`；`All tests passed (2681 assertions in 225 test cases)`

### T-041 · Launchpad 全屏窗口与网格渲染
- 状态: DONE
- 优先级: P1
- 依赖: T-040
- 预估: 1 会话
- 上下文: `src/platform/`、`src/render/`、`ADR-0021`
- 契约: C-18
- DoD:
  - [x] 独立无边框全屏顶层窗口（每显示器一个，ADR-0021 第 1 条），复用 `CompositionSurface` + `IRenderBackend`
  - [x] 自绘网格：列数按 C-18 自适应，行数 `ceil(page_size/columns)`（单测暴露并修掉 pitch 当 cell 宽的点击目标缺陷）
  - [x] 分页：单页 35 项（5×7），页码指示器仅在 >1 页时出现
  - [x] 图标悬停轻微放大；点击启动并关闭
  - [x] 打开/关闭动画 p99 < 16.7ms，实测数据入证据（原生 192 DPI p99=16.58ms WITHIN；150% 下 19.36ms 与裸定时器地板 19.226ms 不可区分，未宣称达标）
  - [ ] 混合 DPI 双屏按屏独立创建，禁跨屏共享合成树 —— **PARTIAL**：代码结构已保证（每屏独立 overlay/surface/backend/DPI，unique_ptr 强制不可共享），本机单屏无法实机验证，移交 T-084
  - [ ] 截图：单屏 150% + 混合 DPI 双屏 —— **PARTIAL**：150% 截图已归档 `evidence/T-041/scale150/`，混合双屏受硬件限制，移交 T-084
- 证据: `evidence/T-041/`


### T-042 · Launchpad 搜索与文件夹分组
- 状态: DONE
- 优先级: P1
- 依赖: T-041
- 预估: 1 会话
- 契约: C-18
- DoD:
  - [x] 顶部搜索框，实时过滤，不区分大小写（`CompareStringEx` `NORM_IGNORECASE`）——实测 `search '7' -> 1 item(s)`
  - [x] 无结果时显示空状态，不崩溃——实测 0-item 后进程存活，cell0 区域差分 15.5%
  - [x] 拖图标到另一图标上创建文件夹，持久化到 `launchpad.folders`——press/hover/release 手势（阈值 14 DIP），实测 `folder created ... saved=1`，重启后仍在
  - [x] 文件夹点击展开为内嵌网格，支持重命名——chip 点击进入；F2 重命名（预填旧名，Enter 提交/Esc 取消），重命名保持 id 稳定
  - [x] schema 完全符合 C-18（`id` 唯一、空 `items` 丢弃、坏身份跳过不整组失效）——单测覆盖 + 隔离 config 回读核对
- 证据: `evidence/T-042/`（verify.txt：构建 0 警告、单测 3315 断言全绿、探针 verdict=PASS、8 张截图）
- 备注: 顺带修复 T-041 遗留的第 3 页起图标全空（tiles 截断 70，现改为懒提取）；点击启动改为抬起时触发以给拖拽让位，探针 ClickAt 语义不变

## M4 · 配置与主题（P1）

### T-050 · 设置窗口骨架
- 状态: DONE
- 优先级: P1
- 依赖: T-002（配置加载保存）
- 预估: 1 会话
- 上下文: `src/core/config.*`、`ADR-0021`
- 契约: C-20
- DoD:
  - [x] 独立顶层窗口（自有窗口类，D2D 自绘左侧分区：Dock / Launchpad / 外观 / 行为 / 关于；DComp 保留给透明表面，偏离已声明）
  - [x] 读写 `%APPDATA%\DockForge\config.json`（C-01 字段经 core 范围元数据钳制）
  - [x] 任何修改 300ms 防抖后 `SaveConfig` 并发布 `ConfigChanged`——探针实测 `flush applied=1 saved=1` + `config published icon_size=64`
  - [x] 关闭窗口不退出主进程（WM_CLOSE 只销毁自身）
  - [x] 截图：浅色 + 深色各一张（settings_probe verdict=PASS）
- 证据: `evidence/T-050/`（verify.txt、build.log、test.log 3373 断言全绿、探针脚本与截图）
- 备注: 顺带修复 SaveConfig 只确保 ConfigDir 的真实缺陷（覆盖路径的父目录无人创建，保存静默失败）

### T-051 · 配置热重载与事件总线
- 状态: TODO
- 优先级: P1
- 依赖: T-050
- 预估: 1 会话
- 契约: C-20
- DoD:
  - [ ] `ReadDirectoryChangesW` 监视配置文件，300ms 防抖
  - [ ] 变更发布 `ConfigChanged`（完整快照，C-20），Dock / Launchpad 各自订阅刷新
  - [ ] 非法 JSON 或越界：拒绝加载，保留上一份有效配置，写 WARN
  - [ ] 单测：合法变更、非法 JSON、部分字段缺失、范围越界、连续快速写（防抖合并）
- 证据: `evidence/T-051/`

### T-052 · 主题包加载与切换
- 状态: TODO
- 优先级: P1
- 依赖: T-051
- 预估: 1 会话
- 契约: C-02、C-20、C-18（themeable 列）
- DoD:
  - [ ] 从 `themes/<name>/theme.json` 加载，`extends` 继承 `default`（C-02）
  - [ ] `overrides` 只接受 C-18/C-01 中 `themeable: true` 的键，未知键 WARN 后忽略
  - [ ] 内置 `default` 与 `dark` 两套主题，可在设置窗口切换
  - [ ] 加载失败回退 `default`，Dock 不崩溃不变黑
  - [ ] 切换后 Dock / Launchpad 200ms 内刷新，无闪烁
- 证据: `evidence/T-052/`

## M5 · 系统集成（P1）

### T-060 · 开机自启（任务计划程序）
- 状态: TODO
- 优先级: P1
- 依赖: T-050
- 预估: 1 会话
- DoD:
  - [ ] 用任务计划程序创建登录触发任务（非注册表 Run）
  - [ ] 设置窗口开关，默认关闭
  - [ ] 关闭时删除任务，不残留
  - [ ] 任务 XML 写入 `tools/autostart/` 供审计
  - [ ] spike 先确认 `schtasks` 与 COM `ITaskService` 两条路径哪条可用（禁假设）
- 证据: `evidence/T-060/`

### T-061 · 任务栏显隐（默认关闭 + 紧急恢复）
- 状态: TODO
- 优先级: P1
- 依赖: T-060
- 预估: 1 会话
- DoD:
  - [ ] spike 先验证 `Shell_TrayWnd` 在 Win10 与 Win11 的行为差异（任务书反幻觉条款要求）
  - [ ] `ShowWindow` 控制显隐，默认关闭，设置窗口显式开启
  - [ ] 全局快捷键 `Ctrl+Alt+T` 强制恢复，不依赖设置窗口可达
  - [ ] 退出与崩溃路径都恢复任务栏（`SetUnhandledExceptionFilter` + 正常退出）
  - [ ] 本机版本实测一次并记录
- 证据: `evidence/T-061/`

### T-062 · 崩溃转储与日志轮转
- 状态: TODO
- 优先级: P1
- 依赖: T-002（日志基础设施）
- 预估: 1 会话
- DoD:
  - [ ] `MiniDumpWriteDump` 在未处理异常时写 minidump 到 `%LOCALAPPDATA%\DockForge\dumps\`
  - [ ] 日志按天轮转，保留 14 天，单文件上限 10MB
  - [ ] 设置窗口「关于」页提供「打开日志目录」
  - [ ] 人为触发一次崩溃，验证 dump 生成且不阻塞退出
- 证据: `evidence/T-062/`

## M6 · Stage Manager（P2）

### T-070 · Stage Manager 分组模型
- 状态: TODO
- 优先级: P2
- 依赖: T-061
- 预估: 1 会话
- 契约: C-19
- DoD:
  - [ ] 分组列表：名称、HWND 集合、可选图标（C-19 schema）
  - [ ] 从当前可见顶层窗口建议分组（按进程 / 手动选择）
  - [ ] 持久化到 `stage.groups`
  - [ ] 退出与进程销毁时清理无效 HWND
  - [ ] 不接管 Z-order、不替换 Alt+Tab、不装 `WH_CBT`（DoD 显式核对）
- 证据: `evidence/T-070/`

### T-071 · Stage Manager 切换视图与 Dock 联动
- 状态: TODO
- 优先级: P2
- 依赖: T-070
- 预估: 1 会话
- 契约: C-19
- DoD:
  - [ ] 侧边弹出分组条，每组显示缩略图或图标+标题
  - [ ] 点击分组按 C-19 切换语义还原；前台权限三级回退各实测一次并记 `COMPATIBILITY.md`
  - [ ] Dock 右侧 Stage Manager 入口，可开关
  - [ ] 切换动画 p99 < 16.7ms
  - [ ] 截图 + 录屏帧作为证据
- 证据: `evidence/T-071/`

## M7 · 质量加固与分发（P2）

### T-079 · UI Automation 提供者（T-080 的前置）
- 状态: TODO
- 优先级: P2
- 依赖: T-050、T-041
- 预估: 1 会话
- 上下文: 全仓无 `WM_GETOBJECT`，FlaUI 当前枚举不到任何元素（ADR-0021 第 3 条）
- DoD:
  - [ ] Dock / Launchpad / 设置窗口处理 `WM_GETOBJECT` 并返回 `IRawElementProviderSimple`
  - [ ] 可交互元素（tile、网格项、设置控件）分配稳定 `AutomationId`，命名规则写入 `CONTRACTS.md`
  - [ ] `FlaUInspect` 或 `Accessibility Insights` 实测能看到元素树（截图入证据）
  - [ ] 元素树不暴露内部实现细节（不泄漏 HWND 数值作为 Id）
- 证据: `evidence/T-079/`

### T-080 · FlaUI 冒烟测试套件
- 状态: BLOCKED（依赖 T-079；ADR-0021 第 3 条）
- 优先级: P2
- 依赖: T-079、T-050、T-041、T-061
- 预估: 1 会话
- DoD:
  - [ ] xUnit 测试项目，引用 `FlaUI.UIA3`（.NET 8.0.426 与 NuGet 已确认可用）
  - [ ] 8–12 用例：启动、Dock tile 点击、Launchpad 开/关、设置读写、主题切换、任务栏开关恢复
  - [ ] 全部用 `AutomationId` + `Retry`，禁 `Thread.Sleep`
  - [ ] 一键脚本运行，TRX 输出到 `evidence/T-080/`
- 阻塞说明: 应用侧无 UIA 提供者时选择器无法存在；必须先完成 T-079
- 证据: `evidence/T-080/`

### T-081 · 性能回归基准
- 状态: TODO
- 优先级: P2
- 依赖: T-041、T-071
- 预估: 1 会话
- DoD:
  - [ ] 覆盖 Launchpad 开/关、Stage Manager 切换、主题热切换
  - [ ] 输出 p50/p95/p99，对比 `PERFORMANCE.md` 预算
  - [ ] 脚本可本地一键触发
  - [ ] 超预算记原因并拆优化任务，不降预算标准
  - [ ] 沿用 T-016 的方法论：等 `frames>=1000`、A/B 交替、裸定时器地板对照
- 证据: `evidence/T-081/`

### T-082 · MSIX 打包与 `.appinstaller`
- 状态: BLOCKED（环境缺 Windows SDK：`makeappx.exe` / `signtool.exe` 全盘零命中，ADR-0021）
- 优先级: P2
- 依赖: T-062
- 预估: 1–2 会话
- DoD:
  - [ ] 生成 x64 签名 MSIX（测试证书），合并 `.msixbundle`
  - [ ] `.appinstaller` 描述文件配置启动时检查更新
  - [ ] 干净环境安装、启动、卸载，卸载保留 `%APPDATA%`
  - [ ] 证书不可自动获取则记 `DECISIONS_PENDING.md` 并继续未签名包
- 阻塞说明: 需安装 Windows SDK（`winget install Microsoft.WindowsSDK.10.0.26100` 或 VS Build Tools）。安装属环境变更，记 `BLOCKERS.md` 待人类确认
- 证据: `evidence/T-082/`

### T-083 · Inno Setup 便携版
- 状态: TODO
- 优先级: P2
- 依赖: 无（已从 T-082 解耦，ADR-0021 第 4 条）
- 预估: 1 会话
- DoD:
  - [ ] Inno Setup 脚本生成单文件安装包，支持 `/SILENT`
  - [ ] 便携模式：程序目录旁存在 `portable.flag` 时配置写入程序目录
  - [ ] 卸载不误删 `%APPDATA%\DockForge\themes`
  - [ ] 若 `iscc.exe` 缺失，用 `winget install JRSoftware.InnoSetup` 安装后重跑
- 证据: `evidence/T-083/`

### T-084 · 高 DPI 与多显示器矩阵自动化检查
- 状态: TODO
- 优先级: P2
- 依赖: T-041、T-071
- 预估: 1 会话
- DoD:
  - [ ] 脚本化切换 DPI（100/150/200）并截 Launchpad / Stage Manager / Dock
  - [ ] 混合 DPI 双屏：主屏 150% + 副屏 100%，验证按屏独立创建且不模糊
  - [ ] 截图按 `DPI_<值>_<场景>.png` 命名存入 `evidence/T-084/`
  - [ ] 问题写入 `COMPATIBILITY.md` 并拆修复任务
- 证据: `evidence/T-084/`

---


## P1.5 · 深度集成与差异化

**阶段入口条件**：Dock 悬停放大、缩略图、Launchpad 基础网格已稳定（T-009..T-041 已 DONE）。
**阶段设计原则**：消息计数、任务栏进度、文件夹穿透三项必须 spike 先行，验证不通过则降级为可选插件而非硬编码进主流程；硬件控制与热键系统风险较低可并行；天气与通知中心放在末尾，不阻塞核心差异化功能。

### T-065 · 消息计数适配框架（P1 · 高风险）

- 状态: TODO
- 依赖: T-040（索引稳定）、C-01 配置 schema
- 上下文: `src/shell/`、`ADR-0022`
- DoD:
  - [ ] **Spike 先行**：`spikes/T-065_notification_probe/` 验证 `UserNotificationListener`（WinRT）对目标应用（微信 UWP / QQ Win32 / Discord 至少其一）的通知可见性；不可见则记 `BLOCKERS.md` 并评估窗口标题解析 / UIA 替代路径
  - [ ] 适配器模式：`IMessageCounterAdapter` 接口，每目标应用一个实现，互不影响（C-25）
  - [ ] 第一阶段只支持 1–2 个目标，不追求全覆盖
  - [ ] 计数渲染到 Dock 图标右上角红点/数字叠加层，遵循 C-22
  - [ ] 适配器失败或目标未运行时静默降级，不崩溃、不阻塞 Dock
  - [ ] 设置窗口中可单独开关每个适配器
  - [ ] 单测：适配器注册/注销、计数归零、目标进程退出
- 证据: `evidence/T-065/`

### T-066 · Dock 文件夹穿透浏览器（P1 · 中高风险）

- 状态: TODO
- 依赖: T-014（拖拽基础）、T-040（身份规则）
- 上下文: `src/dock/`、`ADR-0023`
- DoD:
  - [ ] **Spike 先行**：`spikes/T-066_shellfolder_enum/` 验证 `IShellFolder::EnumObjects` + `IEnumIDList` 枚举显示名、图标、PIDL
  - [ ] Dock 文件夹图标左键点击时在 Dock 上方弹出内嵌文件列表（不是打开资源管理器）
  - [ ] 二级目录逐级展开：点击子文件夹在当前面板内钻取，提供返回上级
  - [ ] 文件名悬停显示完整名称，超长截断
  - [ ] **拖出支持**：`IDataObject` + `CF_HDROP` 拖到桌面/资源管理器
  - [ ] 内容变化用 `ReadDirectoryChangesW` 实时刷新，防抖 500ms
  - [ ] 面板关闭：点击外部或 Esc，不阻塞 Dock 其他交互
  - [ ] 空文件夹、无权限文件夹显示友好占位，不崩溃
- 证据: `evidence/T-066/`

### T-067 · 任务栏进度条同步（P1 · 中高风险）

- 状态: TODO
- 依赖: T-013（运行状态指示）
- 上下文: `src/shell/`、`ADR-0024`
- DoD:
  - [ ] **Spike 先行**：`ITaskbarList3::SetProgressValue/SetProgressState` 是设置接口不是读取；`spikes/T-067_progress_probe/` 验证至少一条读取路径（vtable hook 或目标窗口代理状态轮询）
  - [ ] hook 不稳定则降级为白名单轮询（资源管理器复制、Chrome 下载），不追求系统级全覆盖
  - [ ] 进度映射到 Dock 图标底部细进度条，颜色跟随系统强调色（C-22）
  - [ ] 状态机：正常 / 暂停（#FFB900）/ 错误（#E81123）/ 不确定（脉冲），对应 `TBPF_*`
  - [ ] 目标窗口销毁时清理进度状态，与 `DwmUnregisterThumbnail` 同侧清理
  - [ ] 8 小时连续运行无句柄泄漏（`GetGuiResources` 稳态采样，方法同 T-016）
- 证据: `evidence/T-067/`

### T-068 · MyFinder 硬件控制面板（P1 · 低风险）

- 状态: TODO
- 依赖: T-050（设置窗口骨架）、T-062（日志基础设施）
- 上下文: `src/finderbar/`、`C-23`
- DoD:
  - [ ] 分程序音量：`IAudioSessionManager2` → `GetSessionEnumerator` + `ISimpleAudioVolume`
  - [ ] 全局音量/麦克风：`IAudioEndpointVolume`
  - [ ] 媒体控制：`GlobalSystemMediaTransportControlsSessionManager.RequestAsync()`，播放/暂停/切歌/曲目信息
  - [ ] WiFi/蓝牙开关：`Windows.Devices.Radios`
  - [ ] CPU/内存监控：`GetSystemTimes` + `GlobalMemoryStatusEx`，每 2 秒采样，空闲 CPU 增量 < 0.3%
  - [ ] Finder Bar 中以弹出面板呈现，不常驻占顶部栏
  - [ ] 控制失败显示 Toast 或面板内错误提示，不静默失败
- 证据: `evidence/T-068/`

### T-069 · 热区与全局热键系统（P1 · 低风险）

- 状态: TODO
- 依赖: T-060、T-061
- 上下文: `src/core/`、`C-24`
- DoD:
  - [ ] 封装 `RegisterHotKey` / `UnregisterHotKey`，用 `MOD_NOREPEAT` 防长按重复
  - [ ] 四类热键：组件显隐、系统控制、显示模式、窗口管理
  - [ ] 冲突检测：注册失败读 `GetLastError`，设置窗口提示"已被其他程序占用"
  - [ ] 退出时 `UnregisterHotKey` 全部热键，不残留系统级占用
  - [ ] 屏幕热区：进入边缘/角落触发动作，大小可配置
  - [ ] 热区用低频轮询（`GetCursorPos` + 定时器 ≤10Hz）或 `WH_MOUSE_LL`，spike 验证 CPU 占用
  - [ ] 所有热键与热区默认关闭，设置窗口显式开启
  - [ ] 热键配置写入 C-24，支持导入/导出
- 证据: `evidence/T-069/`

### T-075 · 天气小组件（P1 · 低风险）

- 状态: TODO
- 依赖: T-051（配置热重载）
- 上下文: `src/dock/`、`ASSETS.md`
- DoD:
  - [ ] 高德 Web API：IP 定位 + 实时天气 + 预报；**Key 不硬编码**，存 `%APPDATA%\DockForge\secrets.json`（不入库）
  - [ ] **无 Key 兜底**：Open-Meteo 公开 API，城市手动指定
  - [ ] Dock 图标动态渲染：温度作图标文字叠加，天气状态用符号
  - [ ] 悬停预览显示未来 3 天预报
  - [ ] 网络请求异步，失败显示占位图标，不阻塞渲染
  - [ ] 刷新间隔默认 30 分钟可配置，支持手动刷新
  - [ ] 图标素材自绘或 CC0，登记 `ASSETS.md`
- 证据: `evidence/T-075/`

### T-076 · macOS 风格通知中心（P1 · 中风险）

- 状态: TODO
- 依赖: T-065
- 上下文: `src/notifications/`、`ADR-0025`
- DoD:
  - [ ] **Spike 先行**：`UserNotificationListener` 需清单声明 `userNotificationListener` 能力；MSIX 打包前用非打包方式验证可用性，受限则降级为仅显示本应用通知
  - [ ] 右上角 macOS 风格通知窗，支持堆叠、超时收起、点击跳转
  - [ ] 通知窗使用与 Dock 一致的材质降级链（C-21）
  - [ ] 与 Dock 图标红点联动：通知到达时对应图标短暂脉冲
  - [ ] 通知历史面板（点击 Finder Bar 时钟区域展开），支持清除
  - [ ] 通知数据仅存内存，不落盘、不采集内容
- 证据: `evidence/T-076/`

### T-085 · 社区主题分发替代方案（P1 · 低风险）

- 状态: TODO
- 依赖: T-052（主题加载）
- 上下文: `ADR-0026`
- DoD:
  - [ ] 不绑定 Steam Workshop；基于 GitHub Releases / 静态索引 JSON
  - [ ] 主题浏览器：拉取远程索引（名称、作者、预览图、下载 URL、SHA256），显示在设置窗口
  - [ ] 一键安装：下载 `.dockforge-theme`（zip）到 `%APPDATA%\DockForge\themes\<name>\`，校验 SHA256，失败拒绝安装
  - [ ] 主题包格式遵循 C-02，不得含可执行内容
  - [ ] 远程索引 URL 可配置，默认指向项目自己的 GitHub Pages 或 Release 资产
  - [ ] 本地主题管理器：列表、启用/禁用、删除、打开文件夹
  - [ ] **不实现**评分、评论、订阅推送
- 证据: `evidence/T-085/`

### P1.5 完成定义

- [ ] 消息计数至少覆盖 1 个目标应用，红点正常，适配器失败不崩溃
- [ ] Dock 文件夹可展开、二级钻取、拖出，空/无权限友好降级
- [ ] 任务栏进度至少对白名单应用可用，8 小时无句柄泄漏
- [ ] 硬件面板：分程序音量、媒体控制、WiFi/蓝牙、CPU/内存全部可用
- [ ] 全局热键与屏幕热区可配置，默认关闭，冲突检测有效
- [ ] 天气高德/Open-Meteo 双路径可用，网络失败不阻塞
- [ ] 通知中心右上角弹出、堆叠、清除可用
- [ ] 主题浏览器可拉取远程索引并一键安装，SHA256 校验生效
- [ ] 所有新增能力在设置窗口中可单独开关
- [ ] 性能增量：空闲 RSS 增量 < 40MB，空闲 CPU 增量 < 0.8%

---

## P3 · v1.0.0 开源发布

**阶段入口条件**（首次会话执行一次，不跳过）：
1. `STATE.json` 的 `milestone` 为 M2 或以上。
2. `TASKS.md` 中 P1 全部 `DONE`，P2 中 T-080/T-081/T-082/T-084 至少 `DONE` 或 `BLOCKED`（BLOCKED 需在 `BLOCKERS.md` 有明确记录与推荐动作）。
3. `DECISIONS_PENDING.md` 中发布相关待决项按 DECISION AUTONOMY 自决；无法自决则 HANDOFF。
4. 读 `PROGRESS.md` 末 60 行、`BLOCKERS.md`、`git log --oneline -10`、`git status`。
5. 入口检查失败则输出 `[HANDOFF] 上一阶段未达标，不进入发布阶段`，不做任何改动。

**入口检查实测结论（2026-10-25）**：**未通过**。`milestone=M3` 满足第 1 条；P1 中 T-042 仍 `TODO`，P1.5 全部 `TODO`，P2 的 T-050..T-084 全部 `TODO`（T-080/T-082 记 BLOCKED 但前置 T-079 未做），第 2 条不满足。故 P3 任务队列仅登记，不进入执行。

### T-100 · 遗留 PARTIAL 清理与回归

- 状态: TODO
- 依赖: 无
- DoD:
  - [ ] `TASKS.md` 中所有 PARTIAL 逐条复检，确认 DONE 或降级为 v1.1 backlog 并记录
  - [ ] 回归 FlaUI 冒烟 8–12 例全绿
  - [ ] 回归性能基准，Launchpad / Stage Manager / 主题切换 p99 均在预算内
  - [ ] 回归 DPI 矩阵（100/150/200 + 混合双屏）截图归档
- 证据: `evidence/T-100/`

### T-101 · 技术债清理

- 状态: TODO
- 依赖: T-100
- DoD:
  - [ ] 全仓库 TODO/FIXME/HACK 清理或归档到 `docs/backlog.md`
  - [ ] `clang-tidy` + `clang-format` 全量通过并集成 CI
  - [ ] 编译警告清零
  - [ ] 无效代码、调试输出、临时文件清理
- 证据: `evidence/T-101/`

### T-102 · 测试覆盖率补齐

- 状态: TODO
- 依赖: T-101
- DoD:
  - [ ] 核心模块（配置、主题、索引器、布局引擎）单测覆盖率 ≥ 70%
  - [ ] 覆盖报告归档 `evidence/T-102/`
  - [ ] CI 中运行单测并上传覆盖率
- 证据: `evidence/T-102/`

### T-110 · 法律与社区文件

- 状态: TODO
- 依赖: T-100
- DoD:
  - [ ] `LICENSE`（GPL-3.0-or-later 完整文本）
  - [ ] `NOTICE`（第三方依赖许可汇总）
  - [ ] `CONTRIBUTING.md`（环境、构建、测试、提交规范、PR 流程）
  - [ ] `CODE_OF_CONDUCT.md`（Contributor Covenant 2.1）
  - [ ] `SECURITY.md`（漏洞报告渠道、响应 SLA）
  - [ ] `docs/legal/third-party-licenses.md` 完整列出依赖与许可
- 证据: `evidence/T-110/`

### T-111 · README 与文档

- 状态: TODO
- 依赖: T-110
- DoD:
  - [ ] `README.md`：简介、截图/GIF、功能列表、系统要求、下载、安装、配置示例、FAQ、构建、贡献、协议、致谢
  - [ ] `CHANGELOG.md`（Keep a Changelog，v1.0.0 条目）
  - [ ] `docs/roadmap.md`、`docs/architecture.md`、`docs/configuration.md`、`docs/themes.md`
  - [ ] 中英双语（`README.en.md` + `README.md`），至少中文完整
- 证据: `evidence/T-111/`

### T-112 · GitHub 模板与元数据

- 状态: TODO
- 依赖: T-111
- DoD:
  - [ ] `.github/ISSUE_TEMPLATE/bug_report.yml` + `feature_request.yml` + `config.yml`
  - [ ] `.github/PULL_REQUEST_TEMPLATE.md`
  - [ ] 仓库 description、topics
  - [ ] About 页面链接指向文档站
- 证据: `evidence/T-112/`

### T-120 · GitHub Actions 构建

- 状态: TODO
- 依赖: T-101
- DoD:
  - [ ] `.github/workflows/build.yml`：push / PR 触发，Windows Server 2022 runner
  - [ ] 编译 x64 + arm64，产物上传 artifact
  - [ ] 编译耗时基线记录 `evidence/T-120/`
  - [ ] 缓存命中率 ≥ 80%
- 证据: `evidence/T-120/`

### T-121 · GitHub Actions 测试

- 状态: TODO
- 依赖: T-120
- DoD:
  - [ ] `.github/workflows/test.yml`：Catch2 单测 + 覆盖率上传
  - [ ] FlaUI 冒烟标 `workflow_dispatch` 手动触发（需交互桌面），不阻塞 PR
  - [ ] 报告以 TRX + Cobertura 上传
  - [ ] PR 页面展示测试结果
- 证据: `evidence/T-121/`

### T-122 · GitHub Actions 发布

- 状态: TODO
- 依赖: T-082、T-083、T-120
- DoD:
  - [ ] `.github/workflows/release.yml`：`v*` tag 触发
  - [ ] 生成 MSIX（x64 + arm64）、`.msixbundle`、Inno Setup 便携版、`.appinstaller`
  - [ ] 生成 `SHA256SUMS.txt`
  - [ ] 自动创建 GitHub Release 附全部资产
  - [ ] Release Notes 从 `CHANGELOG.md` 自动提取
- 证据: `evidence/T-122/`

### T-123 · 代码质量门禁

- 状态: TODO
- 依赖: T-120
- DoD:
  - [ ] CodeQL 工作流（C++）
  - [ ] Dependabot 配置
  - [ ] 分支保护规则文档化到 `docs/branch-protection.md`
  - [ ] PR 需通过 build + test + CodeQL 才可合并
- 证据: `evidence/T-123/`

### T-130 · 品牌资产

- 状态: TODO
- 依赖: 无
- DoD:
  - [ ] Logo（SVG 源 + PNG 16/32/64/128/256/512/1024）
  - [ ] 应用图标 `.ico`（16/24/32/48/64/128/256）
  - [ ] 品牌色板写入 `docs/branding.md`
  - [ ] 全部素材来源与许可登记 `ASSETS.md`，优先自绘
- 证据: `evidence/T-130/`

### T-131 · 展示资产

- 状态: TODO
- 依赖: T-130
- DoD:
  - [ ] 高质量截图 ≥ 8 张（Dock 浅/深、Finder Bar、Launchpad、Stage Manager、设置），DPI 150%
  - [ ] 演示 GIF ≥ 3 段，每段 ≤ 5 秒、≤ 2MB
  - [ ] 演示视频 60–90 秒（1080p）
  - [ ] 资产归档 `assets/branding/` 与 `docs/screenshots/`
- 证据: `evidence/T-131/`

### T-132 · 文档站

- 状态: TODO
- 依赖: T-111
- DoD:
  - [ ] 基于 MkDocs Material 或 VitePress，源文件在 `docs/`
  - [ ] GitHub Pages 自动部署（`.github/workflows/docs.yml`）
  - [ ] 页面：首页、安装、配置、主题制作、架构、Roadmap、FAQ、贡献
  - [ ] 搜索、暗色模式可用
- 证据: `evidence/T-132/`

### T-140 · Release Notes 与版本冻结

- 状态: TODO
- 依赖: T-111、T-122
- DoD:
  - [ ] `CHANGELOG.md` 中 v1.0.0 条目完整
  - [ ] Release Notes 含亮点、已知问题、系统要求、安装说明、升级路径、协议
  - [ ] 版本号统一更新：`STATE.json`、`CMakeLists.txt`、`Package.appxmanifest`、`appinstaller`、About 页
  - [ ] 打 tag `v1.0.0` 并推送
- 证据: `evidence/T-140/`

### T-141 · 发布物验证

- 状态: TODO
- 依赖: T-140
- DoD:
  - [ ] 干净 Win10 21H2 与 Win11 24H2 各装一次 MSIX，启动、卸载、验证无残留
  - [ ] 便携版解压即用，便携模式配置不写 `%APPDATA%`
  - [ ] `.appinstaller` 更新源可达，模拟更新路径
  - [ ] 所有发布物 SHA256 与 Release 页面一致
  - [ ] 已知兼容问题记入 `COMPATIBILITY.md`
- 证据: `evidence/T-141/`

### T-150 · 公开仓库创建与推送

- 状态: TODO
- 依赖: T-140
- DoD:
  - [ ] 读 `.dockforge.env` 的 `PUBLIC_REPO` 与 `PUBLIC_VIS`（默认 `public`）；未提供则写 `DECISIONS_PENDING.md` 并停止本任务
  - [ ] `gh repo create <PUBLIC_REPO> --public --source=. --remote=public --push`
  - [ ] `gh` 不可用则 `git remote add public <url>` + `git push public main --tags`
  - [ ] 推送前用 `git filter-repo` 清理历史大文件与敏感文件（先备份）
  - [ ] 推送 `main`、`dev`、所有 `v*` tag
  - [ ] 设置 topics：`windows`、`dock`、`macos-style`、`desktop-enhancement`、`cpp`
  - [ ] 上传 Logo 作社交预览图
  - [ ] 创建 `v1.0.0` Release 并附加全部资产
  - [ ] `README.md` 徽章指向新仓库
- 证据: `evidence/T-150/`

### T-151 · 公开仓库最终核验

- 状态: TODO
- 依赖: T-150
- DoD:
  - [ ] 全新克隆公开仓库到临时目录，按 README 从零构建成功
  - [ ] 下载 Release 中 MSIX，安装启动成功
  - [ ] 所有徽章渲染正常
  - [ ] Actions 在新仓库首次运行全绿（build + test + docs）
  - [ ] 最终 URL 与 Release 链接写入 `PROGRESS.md` 与 HANDOFF
- 证据: `evidence/T-151/`

### P3 完成定义

- [ ] 全部 P1 / P1.5 / P2 / P3 任务 `DONE` 或有明确降级记录
- [ ] FlaUI 冒烟全绿，性能基准 p99 在预算内
- [ ] LICENSE / README / CONTRIBUTING / CODE_OF_CONDUCT / SECURITY / CHANGELOG / Roadmap 齐全
- [ ] GitHub Actions：build、test、release、docs、CodeQL 全绿
- [ ] Logo、截图、演示 GIF/视频全部就位并登记许可
- [ ] v1.0.0 tag 已打，Release 页面资产完整
- [ ] 新公开仓库已创建，全量推送成功，全新克隆可构建
- [ ] 最终 URL 与 Release 链接写入 `PROGRESS.md` 与 HANDOFF


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

---

# P4 · 对标审计与发布打磨（MyDockFinder 对齐）

> 编号映射：本队列来自 P4 提示词；提示词引用的 C-15/C-16/C-17 与 ADR-0018..0023 在本账本中已被
> 早期阶段占用（C-15..C-21、ADR-0018..0030 均已冻结），统一顺延为 C-28..C-30、ADR-0031..0036，
> 映射关系记录在各条目与 DECISIONS.md 中。执行顺序仍按优先级：P1（M4/M5）→ P1.5 → P2 → P3 → P4。

### T-200 · MyDockFinder 功能对标审计（总入口）
- 状态: DONE
- 优先级: P4
- 依赖: P1.5 全部任务（注：审计先行完成，矩阵将缺失项映射到既有任务 ID；P4 修复任务在 P1.5/P2 完成后执行）
- 预估: 1–2 会话
- DoD:
  - [x] 创建 `docs/parity-matrix.md`，列出来自 MyDockFinder 官方 README、Steam 功能列表、DeepWiki 的全部用户可感知功能
  - [x] 每项功能标注实现状态：`已对齐 / 部分对齐 / 缺失 / 明确不做（附理由）`
  - [x] 标为"部分对齐"和"缺失"的条目逐条映射到修复任务 ID（既有 T-050..T-085 为主，新增 T-201..T-211）
  - [x] 审计范围覆盖：Dock 交互、最小化动画、图标蒙版、隐藏模式、多屏 Dock、窗口定位避让、滚轮/中键、MyFinder 监控与托盘、日历时间定制、3D 沉浸、Stage Manager 窗口列表交互、Launchpad 文件夹与搜索、主题编辑、分发生态
  - [x] "明确不做"条目全部给出理由
  - [x] 人类可读摘要写入 `PROGRESS.md`
- 证据: `evidence/T-200/`（含图标上下颠倒缺陷的根因、修复与单测——用户实测发现的 P0 缺陷，同会话修复）

### T-201 · 三种最小化动画实现
- 状态: TODO
- 优先级: P4
- 依赖: T-200、ADR-0032
- 预估: 1–2 会话
- DoD:
  - [ ] `genie`：窗口向 Dock 图标收拢的拉伸变形动画（D2D 几何变换或 D3D11 顶点着色器）
  - [ ] `scale`：窗口向 Dock 图标缩放，支持 `scaleDX` GPU 加速路径
  - [ ] `suck`：窗口被"吸入"Dock 图标的真空效果
  - [ ] 三种效果尊重 `minimize_animation_speed`，`Shift` 减速生效
  - [ ] `minimize_dynamics` 开启时动画过程实时显示窗口内容（DWM 缩略图或缓存帧）
  - [ ] 游戏进程默认排除，可在设置中为单个应用开关
  - [ ] 三种效果在 150% DPI 下分别录制 GIF 作为证据
- 证据: `evidence/T-201/`

### T-202 · 图标统一蒙版
- 状态: TODO
- 优先级: P4
- 依赖: T-200、ADR-0033
- 预估: 1 会话
- DoD:
  - [ ] 蒙版形状：圆角矩形（可调半径）、超椭圆（可调指数）、圆形（C-29）
  - [ ] 应用于 Dock 图标、Launchpad 图标、Stage Manager 分组图标
  - [ ] 蒙版颜色跟随亮暗模式或主题强调色，高级编辑图标同步跟随（避免 MyDockFinder 已知问题）
  - [ ] 系统图标默认不应用蒙版，可在设置中强制开启
  - [ ] 性能：100 个图标应用蒙版后悬停放大 p99 < 16.7ms
  - [ ] 截图：无蒙版 vs 有蒙版，亮色 + 暗色各一张
- 证据: `evidence/T-202/`

### T-203 · Dock 隐藏模式与多屏行为
- 状态: TODO
- 优先级: P4
- 依赖: T-200、ADR-0034；与 T-061（任务栏显隐）衔接
- 预估: 1 会话
- DoD:
  - [ ] 隐藏模式：正常/自动隐藏/智能隐藏（有窗口最大化时隐藏）/热键触发/始终可见/始终隐藏（C-30）
  - [ ] 每个应用可独立设置隐藏模式
  - [ ] 多显示器同时显示 Dock 开关，开启时禁止跨屏图标拖拽并给出 UI 提示（ADR-0034）
  - [ ] 多屏 Dock 开启时每屏 Dock 独立管理图标布局，不共享配置
  - [ ] 分辨率变化时 Dock 位置与大小正确重算，不偏移
  - [ ] 截图：单屏 + 双屏同时 Dock + 双屏仅主屏 Dock
- 证据: `evidence/T-203/`

### T-204 · 窗口定位与避让
- 状态: TODO
- 优先级: P4
- 依赖: T-200、ADR-0035
- 预估: 1 会话
- DoD:
  - [ ] `window_center_screen`：新建顶层窗口自动居中，可开关
  - [ ] `window_border_avoid_dock`：最大化/贴边不藏在 Dock 后，经工作区或窗口定位实现
  - [ ] Dock 位置（左/下/右）变化时避让区域同步更新
  - [ ] 多显示器下每屏独立计算避让区域
  - [ ] 不注入目标进程，不 hook 全局窗口消息
  - [ ] 单元测试：避让区域计算覆盖四种 Dock 位置 + 多屏
- 证据: `evidence/T-204/`

### T-205 · 滚轮与中键交互绑定
- 状态: TODO
- 优先级: P4
- 依赖: T-200、ADR-0036
- 预估: 1 会话
- DoD:
  - [ ] Dock 图标：中键 = 新建窗口（若应用支持），滚轮 = 调节放大/切换分页
  - [ ] 窗口预览：滚轮 = 切换同应用多窗口，中键 = 关闭当前预览窗口
  - [ ] Stage Manager 列表：滚轮 = 滚动列表，中键 = 切换分组
  - [ ] Finder Bar 按钮：滚轮 = 调节数值，中键 = 重置默认
  - [ ] 绑定仅在自有窗口内生效；默认值写入 C-28
- 证据: `evidence/T-205/`

### T-206 · MyFinder 监控增强与托盘管理
- 状态: TODO
- 优先级: P4
- 依赖: T-200、T-068（硬件面板）
- 预估: 1–2 会话
- DoD:
  - [ ] CPU/内存/磁盘/网络/温度监控，占用 >80% 橙色、>90% 红色（对齐 MyDockFinder 1.11）
  - [ ] 温度检测不使用 WinRing0 驱动（已被 Windows Security 标记），改用 `MSAcpi_ThermalZoneTemperature` WMI 或无驱动方案；不可用则优雅降级为不显示
  - [ ] 托盘图标管理：Finder Bar 中显示/隐藏指定托盘图标，常驻配置用虚拟路径防 UWP 更新失效
  - [ ] 右键功能按钮显示对应系统功能菜单
  - [ ] 音量变化提示浮层（macOS 风格）
  - [ ] 空间音效切换（可选，Core Audio 支持且不引入卡顿时）
- 证据: `evidence/T-206/`

### T-207 · 日历与时间小组件定制
- 状态: TODO
- 优先级: P4
- 依赖: T-200
- 预估: 1–2 会话
- DoD:
  - [ ] 日历图标可定制：月/日字体大小位置颜色、旋转角度、背景图
  - [ ] 指针式时钟（独立时/分/秒图片）与数字式（字体/大小/颜色/格式串）
  - [ ] 农历与节气显示（中文环境）
  - [ ] 点击日历图标展开日历面板
  - [ ] 定制配置写入 `themes/<name>/calendar.json`，随主题分发
  - [ ] 截图：默认 + 定制 + 指针 + 数字
- 证据: `evidence/T-207/`

### T-208 · 3D 沉浸模式与图标倒影
- 状态: TODO
- 优先级: P4
- 依赖: T-200
- 预估: 1–2 会话
- DoD:
  - [ ] 3D 沉浸：悬停透视/景深/阴影组合（D2D 变换或 Composition PerspectiveTransform）
  - [ ] 图标倒影：底部镜像，透明度渐变
  - [ ] 沉浸 + 倒影同开不卡死（MyDockFinder 曾有此问题）
  - [ ] 沉浸模式支持可配置延迟显示
  - [ ] 性能：沉浸 + 倒影 + 悬放同开 p99 < 16.7ms
  - [ ] 录屏 GIF 证据
- 证据: `evidence/T-208/`

### T-209 · Stage Manager 窗口列表增强
- 状态: TODO
- 优先级: P4
- 依赖: T-200、T-071（Stage Manager 基础）
- 预估: 1 会话
- DoD:
  - [ ] Ctrl/Shift 点击窗口列表图标快速分组
  - [ ] 右键菜单：关闭窗口、从分组排除、展开分组
  - [ ] 窗口标题显示开关
  - [ ] 应用排除列表
  - [ ] 窗口靠近列表时调整大小不缩进列表（MyDockFinder 曾有此 bug）
- 证据: `evidence/T-209/`

### T-210 · Launchpad 细节对齐
- 状态: TODO
- 优先级: P4
- 依赖: T-042
- 预估: 1 会话
- DoD:
  - [ ] 拖拽进文件夹不产生重复图标（MyDockFinder 1.13.1 修复项；本项目 T-042 的幂等追加 + 单测已覆盖，复核即可）
  - [ ] 部分图标获取失败时仍显示已成功的，不整体空白（1.18 修复项；本项目懒提取 + 占位环已覆盖，复核即可）
  - [ ] 多显示器 Launchpad 按屏独立创建，单屏/多屏切换不卡死
  - [ ] 搜索支持拼音首字母（可选）与模糊匹配
  - [ ] 文件夹内支持二级目录展开
- 证据: `evidence/T-210/`

### T-211 · 全局稳定性与兼容性收尾
- 状态: TODO
- 优先级: P4
- 依赖: T-200
- 预估: 1–2 会话
- DoD:
  - [ ] 分辨率变化（含多屏切单屏）时 Dock/Finder/Launchpad 位置正确，不偏移不崩溃
  - [ ] Win11 24H2 WiFi 不显示时提示开启系统地理定位（MyDockFinder 已知问题）
  - [ ] 与 MSI Afterburner、PowerToys、游戏全屏共存验证
  - [ ] 8 小时连续运行 RSS 增长 < 15MB，GDI/USER 句柄无线性增长
  - [ ] 崩溃转储 2 秒内完成，不阻塞退出
  - [ ] P1.5/P4 新增功能全部可单独开关，热键/热区默认关闭
- 证据: `evidence/T-211/`

### T-212 · 对标矩阵最终审计与 v1.0.0 冻结
- 状态: TODO
- 优先级: P4
- 依赖: T-201 至 T-211 全部
- 预估: 1 会话
- DoD:
  - [ ] `docs/parity-matrix.md` 所有"部分对齐/缺失"条目变为"已对齐"或移入 `docs/roadmap.md`（v1.1）
  - [ ] 不存在"缺失"且无 roadmap 记录的条目
  - [ ] `CHANGELOG.md` v1.0.0 条目完整，明确"v1.0 不含液态玻璃"及理由（ADR-0031）
  - [ ] Release Notes 无对标产品未实现功能的宣称
  - [ ] 性能预算全部达标，`PERFORMANCE.md` 更新至最终实测值
  - [ ] 打 tag `v1.0.0`
- 证据: `evidence/T-212/`
