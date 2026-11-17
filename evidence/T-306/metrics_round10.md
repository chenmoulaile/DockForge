# T-306 度量对照：Beautiful round10 真图 → DockForge 自绘参数

依据 ASSETS.md：Apple 专有素材与 SF Pro/SF Symbols 一律不打包，Beautiful 的截图
只作度量对照（参数与几何无许可负担，代码全部自绘）。本文记录「看了什么、量了
什么、落到什么参数」。

## 对照的素材（时效分层已核对）

| 素材 | 时间 | 用途 |
|---|---|---|
| `D:/Beautiful/docs/evidence/round10-controlcenter.png` | 2026-11-17（commit e3d9fd1 之后，真 Apple logo + MyDockFinder 真状态图标） | 顶栏右区状态图标的真实观感与间距：时钟「11月17日 周二 17:27」、Wi-Fi 图标、电池图标+「87%」、输入法图标、控制中心图标从右往左排列 |
| `D:/Beautiful/docs/evidence/round5/02-inter-zoom-cc-text.png` | 2026-11-08（放大视图，只看字形） | 电池充电态字形（外框内含闪电 bolt）、文本命名「100% · 充电中」「以太网」 |
| `D:/Beautiful/src/Beautiful.App/TopBar/TopBarWindow.xaml.cs` | 代码 | 状态图标 16×16（NewStatusImage(..., 16)）、UpdateBattery 的 BatteryFlag 判定与 255→0 约定（:1112-1137）、每分钟对账定时器、输入法徽标 1s 轮询（StartInputBadge :1272） |
| `D:/Beautiful/src/Beautiful.Core/Network/Wifi.cs` | 代码 | WifiSignal.Bars 的 0-100→0-4 格阈值（:26-33），wlanapi 信号是线性百分比不是 dBm |
| `D:/Beautiful/src/Beautiful.App/Themes/MacOS.xaml` | 代码 | grep 状态图标相关键 0 命中（无battery/network/ime/status 键），故未从此色板取值；警示红沿用 T-302 已冻结的 HIG systemRed #FF453A |

## 量得 → 落地的参数

| 度量（来源） | DockForge 落点 |
|---|---|
| 电池外框约 25×12 DIP（round10 顶栏目测 + 任务规格明文） | `finder_bar.cpp` `kBatteryIconWidthDip=25`，`status_icons.cpp` DrawBatteryGlyph：body = slot 宽×0.92 高=slot 高×0.75（16 DIP 盒内恰为 25×12），右侧 cap 1.5×5，描边 1.3 DIP，圆角 0.28×body_h |
| 电量文字跟在图标右侧（round10「87%」） | `bar_layout.h` RightItem::text，布局 [padding][icon][gap4][text][padding]，绘制时文本居中于 icon 右侧子矩形（finder_bar.cpp Repaint，修复过整盒居中压住图标的碰撞） |
| 充电态外框内有闪电（round5 放大图） | DrawBatteryGlyph charging 分支：body 内三颗阶梯圆点组成之字形 bolt |
| <20% 变红 #FF453A（任务规格=HIG systemRed，T-302 已冻结 kWidgetCriticalColor） | `BatteryGradeForPercent`：[0,20)→Critical，绘制层文本与电量条同染红（bar_status_low.png 截图两态之一） |
| 无电池隐藏（Beautiful UpdateBattery :1113 BatteryFlag 128/255→Collapsed） | `BatteryPresentForFlag`：128/255→absent，BuildRightItems 直接 continue（无盒无空隙）；本机有电池（99%），隐藏态用 DOCKFORGE_STATUS_DEMO=nobattery 定格（bar_status_nobattery.png） |
| WiFi 三段弧（round10 顶栏 + 任务规格明文「三段弧」） | DrawWifiGlyph：顶点圆点 + 三条圆弧各 7 颗圆点（-135°..-45°），C-04 后端只有圆角矩形，圆弧=圆点链（与 dock::DrawWeatherGlyph 对角线同一手法）；强度格数 WifiBarsForQuality（≥100→4/≥75→3/≥50→2/≥25→1） |
| 状态图标 16×16（TopBarWindow.xaml.cs NewStatusImage(size=16)） | `bar_layout.h` kRightIconBoxDip=16（T-307 已有），电池例外走 icon_width_dip=25 |
| 输入法显示转换模式文字（任务规格明文 A/中；Beautiful 用 InputLanguageManager 1s 轮询） | Win32 路径：ImmGetDefaultIMEWnd + WM_IME_CONTROL(IMC_GETCONVERSIONMODE 0x0001)，显示 ImeConversionLabel（NATIVE 位→「中」否则「A」）；点击 IMC_SETCONVERSIONMODE(0x0002) 翻转 NATIVE 位 |
| 连接类型命名（round5「以太网」；Beautiful SystemStatusService 区分 Ethernet/Wireless80211） | GetAdaptersAddresses：OperStatusUp + 有网关，IfType 71(IEEE80211)→Wi-Fi、6(Ethernet)→以太网；IsConnectedToInternet 走 Network List Manager COM（CLSID_NetworkListManager） |
| 面板卡片度量（Beautiful NotificationCenterWindow 家族） | status_popup：宽 320 DIP、根圆角 12、内容卡圆角 10、外边距 12、卡间隙 10（与 calendar_panel 同节奏）；ADR-0008 自绘 tint 板 + ADR-0045 Solid 档（capture 排除会让面板从截图消失） |

## 明确不抄的部分

- MyDockFinder/Apple 的真状态图标位图：全部改为 Direct2D 自绘几何（ASSETS.md 明令禁打包）。
- Beautiful 顶栏的「真 PNG 优先、矢量回退」素材链：DockForge 无素材文件，不适用。
- Beautiful 输入法徽标的语言标签映射（中/EN/あ）：任务规格指定的是转换模式 A/中
  （WM_IME_CONTROL），两者数据源不同，未采用。
