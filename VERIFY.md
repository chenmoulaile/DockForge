# VERIFY.md · DockForge 可验证版本指南

> 本文件由还原冲刺工作流在每次收尾时自动刷新：重建 exe → 对照清单 → 写本文件。
> **当前版本：commit `562fc0d`**（含 T-307 / T-302 / T-302d / T-306 / T-304 五个工作包 + 两轮视觉修复 ADR-0058）
> exe 由最后一轮门禁实机构建（5590 断言 / 566 用例全绿），此后仅有台账文档改动，无源码变更 —— `build\bin\DockForge.exe` 即当前代码。

## 一、怎么跑

- **exe 路径**：`D:\MacDock\build\bin\DockForge.exe`（直接双击即可）
- 启动后应看到：顶部 Finder 栏（AppBar 预留顶条带）、底部 Dock、系统托盘图标
- **退出**：托盘图标右键 → `Quit DockForge`（兜底：任务管理器结束 DockForge.exe）
- 自己重新构建：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Action all`
- 截图取证（可选）：`pwsh -File tools/screenshot.ps1 -Out shot.png -FullScreen`

## ⚠️ 先看这条：Windows 任务栏会被隐藏

本机 `%APPDATA%\DockForge\config.json` 已设 `"hide_taskbar": true`（还原冲刺裁决：本机开启、出厂默认保持关闭）。
启动 DockForge 后 **Windows 任务栏会消失——这是本次还原效果的一部分，不是故障**。
还原方法：设置/托盘开关，或把 config.json 里 `behavior.hide_taskbar` 改回 `false` 后重启 DockForge。
出厂默认是否翻 true 已登记在 `DECISIONS_PENDING.md`「待决-01」，等你拍板。

## 二、逐项验证清单（本冲刺交付的功能）

### Finder 栏右区（T-307 + T-306）
- [ ] 时钟格式 = `11月18日 周三 HH:mm`（24 小时制、带星期）
- [ ] 右簇从左到右：时钟 → 音量 → WiFi → 电池(+%) → 铃铛 → chevron → 放大镜 → 控制中心四方格（贴右缘）
- [ ] 点击时钟 → 月历面板（360×640、圆角 12、今日蓝圈），点击面板外部关闭
- [ ] 电池 <20% 变红；无电池的机器上电池项整体隐藏
- [ ] 点击输入法徽标 → 中/英切换；点击 WiFi / 电池 → 详情卡弹层

### Finder 栏中央（T-302 / T-302d）
- [ ] 默认中央为空（监控行默认不收编，与 macOS 参考一致）
- [ ] 在 config `finderbar.widgets` 里加 `cpu`/`memory`/`disk`/`network` → 出现监控行；>80% 橙、>90% 红
- [ ] 温度组件无传感器时整体隐藏、不崩溃

### 控制中心（T-304）
- [ ] 点右缘四方格 → Sonoma 版式面板（2×2 连接卡 + 竖滑杆 + 亮度行）
- [ ] 音量/亮度滑杆实时生效；WiFi、蓝牙开关真实翻转
- [ ] 点面板外部或 Esc 关闭；系统音量 OSD 浮层不被接管

### Dock（ADR-0058 视觉修复）
- [ ] 行尾：两行小组件（大时钟在上、温度在下）+ 废纸篓 + 分隔线
- [ ] 悬停放大正常；开一个应用 → 图标下方出现蓝色 `#0A84FF` 运行点
- [ ] 玻璃底三段渐变（上亮下暗）、底缘发丝线、下方留约 10px 壁纸
- [ ] 拖拽重排、右键菜单、最小化窗口汇入 Dock

## 三、已知剩余差距（验收员逐像素比对结论，供你复核）

1. 电池 glyph 内部 3 枚深色圆点（参考为 1 枚）
2. 音量喇叭+单弧 20px（参考为喇叭+双弧、约 32px）
3. 铃铛贴片红色偏亮（255,69,58）且贴片内多一枚白点
4. Dock 小组件右侧多一枚约 14px 空心圆环（参考无此元素）
5. 废纸篓为亮色纸团+蓝回收标（参考为灰色半透明网状）
6. 无前台应用时左区只有一枚图标（参考为 Apple 标+应用名+五个常驻菜单；是否有前台时正常未验证）
7. 顶栏底色 13,15,18 vs 参考 23,23,24（约 10 级灰差，轻微）

## 四、证据与对比材料

- 三轮取证截图（原始 / fixed / fixed_r2）：`evidence\fidelity-sprint\`
- 各包证据：`evidence\T-307\`、`evidence\T-302\`、`evidence\T-302d\`、`evidence\T-306\`、`evidence\T-304\`
- 参考图（Beautiful 的 macOS 风格取证）：`D:\Beautiful\.tools\`、`D:\Beautiful\docs\`
- 冲刺报告：运行产物卡片「macOS 极致还原冲刺报告」

## 五、配置与已知环境问题

- 配置文件：`%APPDATA%\DockForge\config.json`（保存即热重载）
- 材质：本机 blur 磨砂档在 capture 环境下呈空渐变（已知环境问题、非产品回归）；若观感异常可切 solid 档对比
- 多显示器策略、8 小时句柄/内存泄漏属长周期 DoD，未在本冲刺验证
