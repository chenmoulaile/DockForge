# ASSETS.md · 素材许可矩阵

> 无许可登记的素材禁止进 `assets/final/`。
> 来源优先级：CC0/MIT/Apache → 开源项目资源（核对 LICENSE 与署名）→ **自绘（代码生成 SVG/PNG，零风险，最优）**。

## 第三方代码依赖

| 文件名 | 类型 | 来源URL | 许可 | 需署名 | 获取方式 | 状态 |
|---|---|---|---|---|---|---|
| third_party/nlohmann/include/nlohmann/json.hpp | header-only 库 v3.11.3 | https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp | MIT | 否 | curl/Invoke-WebRequest 直取 | OK |
| third_party/catch2/catch_amalgamated.hpp | 测试框架 v3.5.4 | https://github.com/catchorg/Catch2/releases/download/v3.5.4/catch_amalgamated.hpp | BSL-1.0 | 否 | 同上 | OK |
| third_party/catch2/catch_amalgamated.cpp | 测试框架 v3.5.4 | https://github.com/catchorg/Catch2/releases/download/v3.5.4/catch_amalgamated.cpp | BSL-1.0 | 否 | 同上 | OK |

## 图标与字体

| 文件名 | 类型 | 来源URL | 许可 | 需署名 | 获取方式 | 状态 |
|---|---|---|---|---|---|---|
| （运行时提取） | 应用图标 | 系统 `.lnk` / `.exe` | 用户本机资产 | 否 | `IShellItemImageFactory` 运行时提取，**不打包 Apple 专有图标** | 设计内 |
| Segoe UI Variable / Segoe UI | 字体 | 系统自带 | 系统授权 | 否 | 直接引用，**禁止打包 SF Pro** | OK |
| （自绘） | Dock 底板/指示器/圆角遮罩 | 代码生成 | 项目自有 | 否 | Direct2D 几何绘制，零素材文件 | 设计内 |
| （自绘） | T-075 天气符号（晴/多云/阴/雾/雨/雪/雷） | 代码生成 | 项目自有 | 否 | `dock::weather` 渲染层：`DrawWeatherGlyph` 由 FillRoundedRect/StrokeRoundedRect/FillCircle 组合，零素材文件，随 DPI 缩放 | T-075 OK |

## 壁纸

| 文件名 | 类型 | 来源URL | 许可 | 需署名 | 获取方式 | 状态 |
|---|---|---|---|---|---|---|
| （无） | 演示壁纸 | — | — | — | 开发期使用系统壁纸；发布前必须替换为原创或 CC0 | TODO(P2) |

## 待人工导入清单

（空）
