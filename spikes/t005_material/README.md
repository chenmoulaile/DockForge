# spikes/t005_material/README.md — 材质与背景捕获 spike（T-005）

本 spike 的唯一产物是**可复现的测量结论**，不是可发布的代码。每个子程序都能独立运行，
结论都由外部像素采样（`tools/sample_pixels.ps1`、`tools/pattern_contrast.ps1`）或
stdout 计时验证，没有一条来自"我记得应该是"。

## 构建

```powershell
& D:\CMake\bin\cmake.exe -S spikes\t005_material -B spikes\t005_material\build `
    -G "MinGW Makefiles" "-DCMAKE_MAKE_PROGRAM=D:/MSYS2/mingw64/bin/mingw32-make.exe" `
    "-DCMAKE_CXX_COMPILER=D:/MSYS2/mingw64/bin/g++.exe"
& D:\CMake\bin\cmake.exe --build spikes\t005_material\build -j 8
```

## 五个子程序

| 程序 | 回答的问题 |
|---|---|
| `spike_t005_material` | 系统材质（Mica / accent acrylic / blur / DwmEnableBlurBehindWindow）在 `WS_EX_NOREDIRECTIONBITMAP` + DComp 窗口上到底生不生效？ |
| `spike_t005_capture` | `BitBlt(GetDC(NULL))` 会不会把本窗口也拍进去？`WDA_EXCLUDEFROMCAPTURE` 能不能把它排除？ |
| `spike_t005_blur` | 自绘磨砂板（捕获→D2D 高斯→圆角裁剪）一帧要多久？ |
| `spike_t005_dupl` | DXGI Desktop Duplication 在本机能不能开？ |
| `spike_t005_capcost` | 一次屏幕捕获 78ms 的常量到底花在哪？ |

## 结论 1 — 系统材质无视圆角与逐像素 alpha

测试几何：窗口 480x200 位于 (200,200)，背后是 8px 棋盘格背景窗（蓝 `0,128,255` / 深蓝 `0,32,96`），
DComp surface 画 24px 圆角、alpha 0.80 的深灰板。

`tools/pattern_contrast.ps1` 在窗口内部 `(240,240,400,120)` 采样相邻像素平均亮度差：

| 材质 | adjacent_contrast | 判定 | 圆角外 (4,4) |
|---|---|---|---|
| 无材质，alpha 0.0（对照） | 9.055 | 棋盘格清晰 | 背景色 |
| 无材质，alpha 0.80 | 1.889 | 部分 | 背景色 |
| `ACCENT_ENABLE_ACRYLICBLURBEHIND` | 0.188 | 已模糊 | **材质色（未被圆角裁掉）** |
| `ACCENT_ENABLE_BLURBEHIND` | 0.205 | 已模糊 | **材质色** |
| `DWMSBT_MAINWINDOW`（Mica） | 0.003 | 完全模糊 | **材质色 243,243,243** |
| `DwmEnableBlurBehindWindow` | 1.169 | 部分 | 背景色 |

三条硬结论：

1. **Mica 与 accent acrylic 确实会模糊**（对比度 9.055 → 0.003 / 0.188），所以它们在这台
   build 26300 的机器上是生效的，不是被静默忽略。
2. **它们无视 DComp surface 的逐像素 alpha**：alpha 0.0（全透明板）时 `mica_clear` 的窗口内部
   仍是 `243,243,243`，角落也是。也就是说材质铺满整个窗口矩形，圆角完全丢失。
3. **`SetWindowRgn` 裁不掉它们**：`--round-region` 与不加 region 的采样逐点相同。

`DWMWCP_ROUND`（DWM 自己的圆角）确实能裁掉——但半径由系统固定（实测 24px 请求下 `(2,2)`
被裁、`(6,6)` 未被裁），不服从 `appearance.corner_radius`（默认 22，范围 0..40）。

## 结论 2 — 捕获会把本窗口拍进去，`WDA_EXCLUDEFROMCAPTURE` 能排除

窗口内画不透明白板，然后 `BitBlt(GetDC(NULL))` 采样窗口中心：

| 条件 | 窗口中心 | 窗口外 |
|---|---|---|
| 不设排除 | `255,0,255`（本窗口的品红板） | `255,0,255` |
| `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | `0,200,0`（背后背景窗） | `0,200,0` |

所以自绘磨砂板必须给 Dock 窗口设 `WDA_EXCLUDEFROMCAPTURE`，否则会把上一帧的输出喂回自己的输入，
形成递归反馈（越模糊越糊）。

`PrintWindow(Progman, PW_RENDERFULLCONTENT)` 在本机返回全黑（`0,0,0`），不可用。

## 结论 3 — `BitBlt` 是固定 ~78ms 的合成器往返，与区域大小无关

`spike_t005_capcost`（320x120 = 38400 像素）：

| 测法 | p50 |
|---|---|
| A `BitBlt` 背靠背 | 77.778 ms |
| B `BitBlt` 间隔 100ms | 51.459 ms |
| C `GetPixel` 直接读屏幕 DC | 77.934 ms |
| D `PrintWindow` 自己的窗口 | 0.736 ms |
| E `BitBlt` 自己的窗口 DC | 0.657 ms |

区域从 160x60 到 2560x480（像素量差 128 倍）耗时都在 77–79ms（`evidence/T-005/capture_bench.txt`）。
**不是带宽，是读合成后桌面的固定同步成本。** 读自己的窗口 DC 只要 0.65ms，说明 GDI blit 本身没问题。

结论：捕获不能放在 UI 线程的每帧路径上，必须搬到工作线程，UI 线程只消费最新的 DIB。

## 结论 4 — DXGI Desktop Duplication 在本机不可用

`IDXGIOutput1::DuplicateOutput` 返回 `0x80070005 E_ACCESSDENIED`。
本机显示适配器包含 `GameViewer Virtual Display Adapter` 与 `MuMu Virtual Display Adapter`
两个虚拟显示器，是访问被拒的最可能原因（虚拟显示适配器通常不支持 duplication）。
`SM_REMOTESESSION=0`、session 1，不是 RDP 场景。

结论：不能把 GPU 捕获作为唯一路径；主路径是工作线程 + `BitBlt`，GPU 捕获作为可选加速。

## 结论 5 — 自绘磨砂板成本

`spike_t005_blur`（640x120，300 帧，5 帧预热）：

```
capture+blur+present ms: avg=77.892 p50=77.877 p95=79.818 p99=81.094
BitBlt phase ms:         avg=76.766 p50=76.704 p99=79.979
verdict=NEEDS_CACHING
```

99.5% 的时间是 `BitBlt`。D2D 高斯 + 圆角 layer + tint + 描边 + present 合计 < 2ms，
完全在 16.7ms 预算内。**只要把捕获移出 UI 线程，磨砂板就能 60 FPS 渲染。**

## 对主工程的要求

1. Dock 窗口必须 `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`。
2. 磨砂板自绘，不依赖系统材质——因为系统材质不服从 `corner_radius`。
3. 屏幕捕获必须离开 UI 线程，用双缓冲把 DIB 交给渲染线程。
4. `MaterialLevel` 的语义改为"板本身的模糊强度与不透明度档位"，不是"用哪个系统材质"。
5. 系统材质仍可选开启（`appearance.system_backdrop`），但仅作为矩形板背后的底衬，
   一旦 `corner_radius > 0` 就默认关闭。
