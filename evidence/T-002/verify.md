# evidence/T-002 · core: 日志系统 + 配置加载/保存

日期：2026-10-23
模块：`src/core/config.{h,cpp}`（新增）、`src/core/log.{h,cpp}`（沿用 T-001）、`src/main.cpp`（接线）

## DoD 核对

| # | 验收项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 日志写入 `%LOCALAPPDATA%\DockForge\logs\dockforge-YYYYMMDD.log`，格式符合 C-07 | PASS | `runtime.log`，行样例 `2026-10-23T23:22:44.733+08:00 INFO [core] config loaded from ...` |
| 2 | 首次运行生成默认 config.json（11 个键齐全） | PASS | `runtime.log` 中 `config missing, writing defaults`；生成的 `default-config.json` 含 dock 7 键 + appearance 2 键 + behavior 2 键 |
| 3 | 越界值钳制并 WARN；损坏 JSON 回退默认并另存 `.bad` | PASS | 见下方两段实测日志 |
| 4 | 未知键写回时保留 | PASS | `normalized-config.json` 保留 `dock.future_flag` 与顶层 `my_extension` |
| 5 | Catch2 单测覆盖默认值/钳制/损坏回退/未知键保留 | PASS | `test.txt`：**13 用例 92 断言全通过** |
| 6 | `evidence/T-002/test.txt` 含全部用例通过输出 | PASS | 同目录 |

## 实测 1 · 越界 + 未知键（输入 → 回写）

输入：
```json
{ "dock": { "icon_size": 500, "gap": -5, "magnification": 9.0, "future_flag": true },
  "appearance": { "material": "acrylic", "corner_radius": 999 },
  "my_extension": { "a": 1 } }
```
日志：
```
WARN [core] config dock.icon_size=500 out of range [32,96], clamped to 96
WARN [core] config dock.gap=-5 out of range [0,32], clamped to 0
WARN [core] config dock.magnification=9 out of range [1,2.5], clamped to 2.5
WARN [core] config appearance.corner_radius=999 out of range [0,40], clamped to 40
INFO [core] config normalized, rewriting ...
```
回写结果（`normalized-config.json`）：钳制值落盘，`future_flag` 与 `my_extension` 原样保留。

## 实测 2 · 损坏 JSON

输入 `{ "dock": { "icon_size": 48, BROKEN`：
```
ERROR [core] config parse failed ([json.exception.parse_error.101] parse error at line 1, column 33 ...), falling back to defaults
WARN  [core] corrupt config preserved at ...\config.json.bad
```
`config.json` 被重写为默认值，原损坏内容完整保存在 `config.json.bad`。

## 设计要点

- **未知键不丢**：`StripUnknownKeys` 只抽取 schema 不拥有的键，存入 `Config::unknown_keys_json`；`SerializeConfig` 在写完 schema 键后回填，二次 round-trip 稳定（单测 `unknown keys survive a load/save round trip` 断言 `SerializeConfig(again) == saved`）。
- **自愈回写**：`Config::needs_rewrite` 在钳制或枚举/类型回退发生时置位，`LoadConfig` 随即回写磁盘，保证文件内容与应用实际行为一致（用户改坏一个数字不必手工修正）。
- **原子写**：`WriteWholeFile` 写 `.tmp` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`，中途崩溃不会留下半截配置。
- **浮点不入噪声**：`RoundForJson` 把 `1.8f` 序列化为 `1.8` 而非 `1.7999999523162842`（实测首版即暴露此问题，已修）。
- **MinGW 路径限制**：`std::ifstream` 不接受 `std::wstring`，文件 IO 走 `CreateFileW`/`ReadFile`/`WriteFile`。

## 遗留

- `behavior.autostart` / `behavior.hide_taskbar` 目前只解析不使用，实际生效在 M5（T-0xx 系统集成）。
- 热重载（`ReadDirectoryChangesW` + 300ms 防抖）属 T-0xx（M4 配置与主题），本任务只做加载/保存。
