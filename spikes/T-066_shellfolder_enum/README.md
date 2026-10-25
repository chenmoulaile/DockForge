# T-066 spike · IShellFolder::EnumObjects 在 MinGW-w64 下的可行性实测

日期：2026-10-25。日志：`evidence/T-066/spike_shellfolder_enum.log`（14/14 PASS）。

## 问题（TASKS.md DoD 1）

`IShellFolder::EnumObjects` + `IEnumIDList` 能否在 MinGW-w64（无 MSVC、无 C++/WinRT）
枚举显示名、图标、PIDL？失败路径（空/无权限/不存在）形态为何？拖出所需的
`SHCreateDataObject` 是否可用？

## 构建（Git Bash，仓库根）

```
g++ -std=c++17 -O0 -g -o spikes/T-066_shellfolder_enum/spike.exe \
    spikes/T-066_shellfolder_enum/spike.cpp -lole32 -lshell32 -lshlwapi -luser32 -lgdi32
```

注意：`StrRetToBufW` 在 `shlwapi.h`（不在 shlobj.h），MinGW 头需要显式 include。

## 结论（全部实测）

| 项 | 结果 |
| --- | --- |
| EnumObjects + IEnumIDList | S_OK，3 子项（子目录+2 文件） |
| SIGDN_NORMALDISPLAY 显示名 | 非空，逐项验证 |
| SIGDN_DESKTOPABSOLUTEPARSING 完整路径 | 全部以目录前缀开头 |
| SFGAO_FOLDER 属性 | 子目录=folder，文件=非 folder |
| HICON（SHGFI_PIDL\|SHGFI_ICON） | 3/3 非空（用后 DestroyIcon） |
| 系统图像列表索引（SHGFI_SYSICONINDEX） | 3/3 >= 0 |
| 绝对 PIDL 字节数 | 3/3 > 0（462B，真实多级列表） |
| 空文件夹 | **S_OK + 0 子项**（不是错误）→ 占位"空文件夹" |
| C:\System Volume Information | **0x80070005** ACCESS_DENIED → 占位"无权限" |
| 不存在路径 | **0x80070002** → 占位"不存在" |
| SHCreateDataObject(父 PIDL, 1 子 PIDL) | S_OK → IDataObject |
| QueryGetData(CF_HDROP) | S_OK（拖出载荷就绪） |
| 耗时 | 冷 215ms / 热（进程内二次）≈0ms；生产实测 reload 5-20ms |

## 生产落地

- 逻辑层：`src/shell/folder_browser.{h,cpp}`（ListFolder/SortFolderEntries/
  FolderNavStack/FolderStatus 映射），单测 `tests/folder_browser_test.cpp`
- 面板：`src/dock/folder_panel.{h,cpp}`（GDI 弹窗、钻取/返回/Esc、拖出
  DoDragDrop、LaunchpadWatcher 复用监视）
- 证据链：`tools/folder_panel_probe.ps1`（A..K 11 部分）→ `evidence/T-066/`
