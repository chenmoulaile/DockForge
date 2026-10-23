# Spike T-004 · DirectComposition 分层窗口

独立可编译工程，验证 DockForge 的渲染底座。**结论已并入主工程**（`src/platform/composition.*`）。

## 构建与运行

```powershell
$env:PATH = "D:\MSYS2\mingw64\bin;D:\CMake\bin;$env:PATH"
cd spikes\t004_dcomp
cmake -S . -B build -G "MinGW Makefiles" `
  "-DCMAKE_TOOLCHAIN_FILE=D:/MacDock/cmake/toolchain-mingw.cmake" `
  "-DCMAKE_MAKE_PROGRAM=D:/MSYS2/mingw64/bin/mingw32-make.exe" `
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmake --build build -j 8

# 运行（控制台输出逐步 HRESULT）
.\build\spike_t004_dcomp.exe --mode=noredir --backdrop --dpi=192 --hold-ms=5000
```

参数：`--mode=plain|noredir|layered|both`、`--dpi=<96|144|192|...>`、`--backdrop`（在窗口下方铺纯色背景窗）、`--hold-ms=<ms>`。

## 结论 1：DComp 设备必须由 ID2D1Device 创建

`DCompositionCreateDevice2` 传 `IDXGIDevice` 时，整条链路直到 `Commit()` 全部返回 `S_OK`，只有 `IDCompositionSurface::BeginDraw(nullptr, IID_ID2D1DeviceContext, ...)` 返回 `E_NOINTERFACE (0x80004002)`。

```
DCompositionCreateDevice2                  hr=0x00000000 OK
IDCompositionDesktopDevice::CreateTargetForHwnd hr=0x00000000 OK
IDCompositionDevice2::CreateSurface        hr=0x00000000 OK
IDCompositionTarget::SetRoot               hr=0x00000000 OK
IDCompositionSurface::BeginDraw            hr=0x80004002 FAIL   <-- 传 IDXGIDevice 时
```

传 `ID2D1Device` 后 `BeginDraw` 成功，且回填的 `ID2D1DeviceContext` 直接可用。**这是主工程 `CompositionRenderer::Initialize` 的硬约束**：先建 `ID2D1Device`，再建 DComp 设备。

## 结论 2：三种扩展样式都能做到逐像素透明

窗口 240x90 DIP、圆角 20 DIP，在 192 DPI 下物理 480x180、圆角 40px。背景铺 `RGB(0,128,255)`，采样：

| 模式 | 中心 | 角落 (4,4) | 上边缘中点 |
|---|---|---|---|
| `WS_EX_NOREDIRECTIONBITMAP` | (250,56,64) 红 | (0,128,255) 蓝 = 透 | 红 |
| `NOREDIRECTIONBITMAP\|LAYERED` | 红 | 蓝 = 透 | 红 |
| `LAYERED` | 红 | 蓝 = 透 | 红 |
| `PLAIN`（无扩展样式） | 红 | 蓝 = 透 | 红 |

圆角弧线也正确：`(4,30)` 与 `(30,4)` 已进入红色区域，`(4,20)` 与 `(20,4)` 仍在蓝色区域——半径 40 的圆弧边界与几何一致。

**选型**：用 `WS_EX_NOREDIRECTIONBITMAP` 单独一种。`WS_EX_LAYERED` 在本场景无收益，且会强制 DWM 走分层路径、增加一次合成拷贝；两者同时设置同理。ADR-0007 记录该决策。

## 结论 3：DPI 缩放在 D2D 侧完成，窗口尺寸同步

内容在 DIP 下布局，`ID2D1DeviceContext::SetDpi(dpi,dpi)` 负责转物理像素；窗口尺寸按 `lround(logical * dpi/96)` 设置。三档实测：

| 强制 DPI | scale | 物理尺寸 | 中心采样 | 角落 |
|---|---|---|---|---|
| 96 | 1.000 | 240x90 | 红 | 透 |
| 144 | 1.500 | 360x135 | 红 | 透 |
| 192 | 2.000 | 480x180 | 红 | 透 |

`GetDpiForWindow` 在 192 DPI 屏幕上返回 192，与 `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` 一致。

## 结论 4：置顶窗口的 z 序陷阱

首轮测试窗口与背景窗都置顶时，采样读到的全是背景色——同 `WS_EX_TOPMOST` 的两个窗口，后创建者不一定在上。修正为：Dock 窗口 `WS_EX_TOPMOST`，背景窗不置顶。这直接决定了主工程里 Dock 与 Finder Bar 的 z 序管理方式（都置顶时靠 `SetWindowPos` 显式排序，不依赖创建顺序）。

## 链接依赖（MinGW）

`GetDpiForMonitor` 在 `shcore`，`DCompositionCreateDevice2` 在 `dcomp`，`D3D11CreateDevice` 在 `d3d11`。MinGW 下 `d2d1`/`dcomp`/`dxgi`/`d3d11`/`shcore` 导入库均存在，无需 MSVC。
