# T-304 度量对照 · Beautiful ControlCenterPanel 参数记录

来源（只取参数/度量，代码自绘，ASSETS.md 合规；未复制任何素材文件）：

- `D:/Beautiful/src/Beautiful.App/Controls/ControlCenterPanel.xaml`（851 行，逐行读取）
- `D:/Beautiful/src/Beautiful.App/Themes/MacOS.xaml`（63 行，逐行读取）
- `D:/Beautiful/src/Beautiful.App/Themes/MacTokenDictionary.cs` / `Beautiful.Core/Design/MacTokens.cs`（token 值）
- `D:/Beautiful/src/Beautiful.App/Services/AcrylicWindow.cs:29-30`（24H2 ACCENT_ENABLE_ACRYLICBLURBEHIND 不模糊且 tint 平铺满窗的实测教训 → 材质一律 ADR-0008 自绘）
- `D:/Beautiful/docs/evidence/round10-controlcenter.png`（形态对照；按任务简报的时效分层，round4 旧图未采信图标观感）

| 度量 | Beautiful 来源 | MacDock 落地（hardware_panel.h 常量） |
|---|---|---|
| 内容卡宽 | Card Width=348（xaml:656） | `kCcPanelWidthDip = 348` |
| 面板圆角 | MacOS.xaml `ThemePanelCornerRadius=18`（xaml:28） | `kCcPanelRadiusDip = 18` |
| 卡描边 | BorderBrush #16FFFFFF（xaml:656） | `kStroke = {1,1,1,0x16/255}` 1 DIP |
| 连接卡圆角 | CcConnCard CornerRadius=16（xaml:456） | `kCcCardRadiusDip = 16` |
| 组行圆角 | CcGroupStyle CornerRadius=12（xaml:446） | `kCcGroupRadiusDip = 12` |
| 滑杆瓦片圆角/宽 | CcSliderTile CornerRadius=14 Width=46（xaml:621-628） | `kCcTileRadiusDip = 14` `kCcVolumeTileWidthDip = 46` |
| 连接圆钮 | 28×28 CornerRadius=14（xaml:478-479），off #3DFFFFFF / on ThemeAccent #0A84FF | `kCcConnButtonDip = 28`，`kButtonOff`/`kAccent` 同值 |
| 竖滑杆 | Slider Width=26 Height=118（xaml:68-70），可视轨 Border Width=20 CornerRadius=10（xaml:82） | `kCcSliderWidthDip = 26` `kCcSliderLengthDip = 118` `kCcSliderTrackWidthDip = 20` |
| 拇指 | Thumb 26×4（xaml:90），视觉透明（填充段即把手，xaml:26-28 注释） | `kCcThumbDip = 4`，画为填充上缘的亮色把手带 |
| 内容边距 | StackPanel Margin=16,10,16,12（xaml:665） | `kCcSidePadDip=16` `kCcTopPadDip=10` `kCcBottomPadDip=12` |
| 组间距 | Grid Margin=0,0,0,8 节奏（xaml:679 等） | `kCcSectionGapDip = 8` |
| 卡底/边 | AurCardFill #14FFFFFF / AurCardBorder #1FFFFFFF（MacTokenDictionary.cs:116-118） | `kCardFill` / `kCardBorder` 同值 |
| 玻璃 tint 基准 | MacOS.xaml 三段 #C838383A(0)→#B8242426(0.5)→#A8161618(1)（xaml:15-19）+ 浮层 tint #4D202020（xaml:25） | D2D 线性渐变 brush + FillRoundedRect 叠加，同值 |
| 面板高度 | （Beautiful 无固定高，MacDock 由常量推导） | `HardwarePanelHeightFor() = 10+168+8+46+8+22+12 = 274`（单测钉住） |
| 弹层底色裁定 | ControlCenterPanel.xaml:16「macOS 主题恒为深色玻璃」 | 双系统主题恒深玻璃白字，同裁定 |

与规格的差异说明（规格原文 vs 本机事实）：

1. 规格写「竖滑杆（音量 26×118/thumb 26×4）+ 亮度行」——音量竖滑杆 26×118/thumb 26×4 逐参数落地；亮度为横滑行（Beautiful 的横向胶囊样式 CcMacSliderHorizontal Height=26/轨 20/thumb 4×26 的参数语言），因规格 ② 明文「亮度行」。
2. 规格提示 DDC/CI `SetMonitorBrightness`——spike（spikes/T-304_cc/ddc_registry_spike.txt）实测本机内屏 `GetPhysicalMonitorsFromHMONITOR` 失败 gle=1168（ERROR_NOT_CONNECTED，eDP 面板不走 DDC），而 WMI `WmiMonitorBrightness` 可读（CurrentBrightness=62/Levels=101，wmi_brightness_probe.txt）。实现 WMI 优先、DDC/CI 兜底双路径（ADR-0057），本机取证走 WMI 实时生效。
3. 勿扰/夜览注册表：spike 实测本机 `PushNotifications\ToastNotificationEnabled` 与夜览 CloudStore 键均不存在（r=2）。勿扰写路径创建键/值并回读成功（ToastNotificationEnabled 1→0→复原）；夜览键缺失 → 面板内提示（规格 DoD「失败面板内提示」口径），blob 翻转逻辑（0x0F↔0x13 @0x12）以单测钉住、有键机器可走通。
