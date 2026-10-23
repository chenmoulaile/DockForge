# CONTRACTS.md · 冻结契约

> 变更规则：任何跨模块接口一经写入本文件即冻结。修改必须新增 ADR（DECISIONS.md）并显式迁移（记录迁移任务 ID）。

---

## C-01 配置 Schema v1

配置文件：`%APPDATA%\DockForge\config.json`（UTF-8，JSON）

| 键 | 类型 | 默认 | 范围/枚举 | 单位 |
|---|---|---|---|---|
| dock.icon_size | int | 48 | 32..96 | px@100%DPI |
| dock.gap | int | 8 | 0..32 | px@100%DPI |
| dock.magnification | float | 1.8 | 1.0..2.5 | 倍数 |
| dock.hover_falloff | enum | sine | sine/cosine_integral/gaussian | - |
| dock.influence_radius | float | 3.0 | 1.0..5.0 | 图标宽倍数 |
| dock.falloff_sharpness | float | 2.0 | 0.5..4.0 | 指数 p |
| dock.position | enum | bottom | bottom/left/right | - |
| appearance.material | enum | mica | mica/acrylic/blur/solid | - |
| appearance.corner_radius | int | 22 | 0..40 | px@100%DPI |
| behavior.autostart | bool | false | - | - |
| behavior.hide_taskbar | bool | false | - | - |

**实现约束**
- 未知键：忽略但保留（写回时不得丢失）。
- 越界值：钳制到范围并在日志 WARN，不抛异常。
- 解析失败：整份回退默认值，保留损坏文件为 `config.json.bad`。

## C-02 主题包

```
themes/<name>/theme.json + preview.png(512x288) + icons/
theme.json: { name, author, version, extends, overrides:{<C-01 子集>} }
```

## C-03 目录约定

- 配置：`%APPDATA%\DockForge\config.json`
- 主题：`%APPDATA%\DockForge\themes\`
- 日志：`%LOCALAPPDATA%\DockForge\logs\dockforge-YYYYMMDD.log`
- 转储：`%LOCALAPPDATA%\DockForge\dumps\`

## C-04 渲染后端契约（T-004 冻结）

- 后端接口 `dockforge::render::IRenderBackend`，由 `src/render/backend.h` 定义。
- 所有坐标在**逻辑像素**（DIP）下计算，paint 时由后端乘以 `scale`。
- 后端必须在 `resize(w,h)` 后一帧内清屏，不得残留上一帧内容。

## C-05 Dock 布局数学（T-008 冻结）

对图标 i（中心 x 坐标 c_i，宽度 w），鼠标 x = m：

```
d_i = |m - c_i|
R   = influence_radius * w
t   = clamp(d_i / R, 0, 1)
s_i = 1 + (magnification - 1) * (1 - t)^falloff_sharpness   // 简化锐度曲线，见 ADR-0004
x_i = base_i + Σ_{j} (s_j - 1) * w * 0.5 * sign(i - j)
```

- 缩放与位移**只在顶点缓冲/变换矩阵中体现**，禁止逐帧重建布局对象。
- 帧时间预算 p99 < 16.7ms。

## C-06 模块边界

| 模块 | 职责 | 不得触碰 |
|---|---|---|
| src/platform | Win32/DWM/Composition 封装、DPI、窗口类注册 | 业务状态 |
| src/render | 材质、模糊、圆角、动画曲线、绘制后端 | 窗口消息循环 |
| src/core | 配置、日志、事件总线、状态机 | 直接 GDI/D2D 调用 |
| src/shell | 图标提取、.lnk 解析、窗口枚举、缩略图 | UI 绘制 |
| src/dock | Dock 布局与交互 | 直接 P/Invoke 之外的系统调用 |
| src/finderbar | 顶部栏 | Dock 状态 |

## C-07 日志格式（T-002 冻结）

```
<ISO8601 本地时间带偏移> <LEVEL> [<module>] <message>
例：2026-01-01T12:00:00.123+08:00 INFO  [core] config loaded from ...
```
LEVEL ∈ TRACE/DEBUG/INFO/WARN/ERROR/FATAL。文件按天切分，保留 7 天。
