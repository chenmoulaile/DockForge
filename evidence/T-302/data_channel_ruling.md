T-302 数据通道裁定与切片范围（evidence 记录，DoD 2/8）
========================================================

一、数据通道裁定（详见 DECISIONS.md ADR-0054）

裁定: 不扩 C-23 冻结载荷、不改其采样节律；bar_widgets 自持采样。

依据（逐条可复核）:
1. CONTRACTS.md C-23（P1.5 冻结）SystemStatsUpdated 载荷仅 cpu_percent /
   memory_percent / memory_used_bytes / memory_total_bytes 四字段，采样 ≤0.5Hz，
   明令禁止在渲染路径上采样（CONTRACTS.md:995-1011）。
2. system_stats.h:1-9 头注释自证 StatsRow/C-23 只有 CPU/内存（T-068 口径）；
   hardware_types.h:39-45 SystemStatsUpdated 结构与冻结文本逐字段一致。
3. C-23 事件不可行依据: hardware_panel.h:47 `Open() // show + start ticking` ——
   SystemStatsUpdated 仅面板打开时广播（hardware_panel.cpp:421-425 的 2s
   累积器在面板可见时才走）。栏内小组件需要常驻数据流，面板事件给不了。
   扩载荷需契约变更 + ADR，收益不抵成本。
4. 采样归属（全不在渲染路径）:
   - 轻量四类（CPU/内存/磁盘/网络）归 finderbar Tick 链
     （finder_bar.cpp FinderBar::Tick → bar_widgets.cpp WidgetHost::Tick），
     周期默认 2000ms（= C-23 的 0.5Hz；config 钳制下限 2000 使手改文件也
     无法突破节律上限）。单次采样 API: GetSystemTimes 差分（复用
     system_stats.cpp:37 ReadSystemCounters）/ GlobalMemoryStatusEx /
     GetDiskFreeSpaceExW / GetIfTable2（微秒级）。
   - 温度 WMI（MSAcpi_ThermalZoneTemperature，禁 WinRing0——T-206 DoD 明令）
     查询有阻塞成本，放独立工作线程（bar_widgets.cpp ThermalThreadMain，
     std::thread），结果经原子快照（std::atomic<int32_t> 毫摄氏度）交换给
     渲染读取，无锁；失败退避 15s 重连，不可用时该组件不显示。
   - C-23 的「禁止渲染路径采样」针对 Paint；Tick 链采样与其精神一致
     （hardware_panel 自己的采样也挂在 Tick 上，hardware_panel.cpp:421）。

降级方案（未触发，仅记录）: 若实测超标，CPU/内存改读 system_stats 共享快照、
仅磁盘/网络/温度自采样。实测 idle_on − idle_off = −0.0057pp、
idle_dual − idle_off = +0.0462pp，均 < 0.3%（cpu_budget.txt），无需降级。

二、色值出处（复核意见 wrong-3 修订）

橙 #FF9F0A / 红 #FF453A 取 macOS HIG systemOrange / systemRed 标准值（dark 模式
值；栏板为固定深 tint，两主题共用）。已 grep Beautiful MacOS.xaml:
  grep -c "9F0A" MacOS.xaml → 0；"453A" → 0；"Orange" → 0；"Red" → 0
即 Beautiful 无任何橙/红警示色键，无从取值，亦不冒认出处。
Beautiful 侧可引用的参数（AppSettings.cs，读后采纳）:
  - 温度无数据源时芯片整体隐藏、绝不显示编造读数（AppSettings.cs:354）——
    采纳为本包 temperature 无效即不渲染的规则。
  - Beautiful 温度轮询默认 5s（AppSettings.cs ThermalRefreshSeconds，
    钳 5..120）——本包刷新周期按 T-302 规格钉 2s（规格优先），温度线程随
    同一周期；两者口径差异已在此注明。
  - MSAcpi_ThermalZoneTemperature.CurrentTemperature 单位 0.1K（TemperatureReader
    .cs:59 KelvinTenthsToCelsius）——换算口径一致（/10 − 273.15）。

三、本包切片范围（T-302 第一会话切片，跨两包验收）

已落地（本包）:
  - bar_widgets.{h,cpp}: 注册表（C-31 八类型名全部可解析；cpu/memory/disk/
    network/temperature 五类渲染器注册，weather/media/calendar_brief 留给
    T-302d，未注册类型跳过不占位）+ 栏内单行紧凑绘制（28 DIP 内、13 DIP 字号
    = kMenuFontDip）+ 色阶三档映射（独立可单测）+ 采样引擎（Tick 链 + 温度线程）
  - config: finderbar.widgets（C-31 冻结键形 [{type,order}]）与
    finderbar.widgets_refresh_ms（新键，ADR-0054）解析/钳制/往返/quiet-file
  - finder_bar.{h,cpp} 接入: 中央区布局（左右两区之间居中、越界丢盒不挤压）+
    渲染（Normal 用主题前景、Warn/Critical 用冻结 HIG 值）+ Tick 链采样 + 配置
    热重载 re-arm + state JSON（probe 可断言 widgets.boxes/grade/snapshot）
  - 单测: tests/t302_widget_tests.inc 17 用例（色阶/文本/纯函数/布局/config）
  - 取证: 三档色阶截图、默认态真实采样截图、热重载往返、三场景 CPU 预算

留给 T-302d（DoD 语义不变）:
  - weather / media / calendar_brief 三类渲染器（注册位已留: WidgetTypeRegistered）
  - 小组件增删排序 UI
  - 「截图三张」中的「精简/全量」两张全景验收截图（本包已产 reduced 态与
    default 态截图，可复用；全量态待三类渲染器齐后重拍）

四、本机温度 WMI 实况

  powershell Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature
  → "拒绝访问"（wmi_thermal_check.txt）
  应用侧一致: state_default.txt temperature_valid=false，温度组件不渲染
  （default 模式 4 盒），进程不崩溃；日志一行
  "WMI temperature unavailable (MSAcpi_ThermalZoneTemperature); the temperature
  widget renders nothing on this machine"。
  三档截图中的 TEMP 读数来自取证专用环境变量 DOCKFORGE_WIDGETS_DEMO（
  normal=45°C / warn=85°C / critical=95°C），仅为截图确定化；生产默认不启用。
