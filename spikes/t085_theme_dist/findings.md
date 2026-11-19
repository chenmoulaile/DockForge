# T-085 spike findings · 主题分发链路（MinGW）

日期: 2026-11-19/20 · 编译/运行: Git Bash + `powershell -File spikes/t085_theme_dist/spike_driver.ps1`
spike 本体: `spike.cpp`（Q1-Q5 自证用例）+ `spike_driver.ps1`（Compress-Archive 造包 + HttpListener 回环源）
日志: `spike_run.log`（最终一轮 Q1..Q5 全 PASS，见下）

```
Q1 PASS sha256 vectors (abc, empty)
Q2 PASS download 1204 bytes sha-ok elapsed=16..31ms; 404 status=404 0ms; refused err=12029 ~2031ms
Q3 PASS raw.githubusercontent.com status=200 bytes=6034 elapsed=234..1078ms err=0
Q4 PASS miniz extract files=3 0ms (theme.json+preview.png+icons/sample.png)
Q5 PASS theme.json 121 bytes: { "name": "mono", ... }
ALL PASS
```

## 结论（按 DoD 需要的每条路径）

1. **bcrypt SHA256（Q1）可用**：MinGW 直连 `BCryptOpenAlgorithmProvider/BCryptCreateHash/BCryptHashData/BCryptFinishHash`，
   RFC 6234 "abc" 与空串两个已知向量全对；对下载体（1204B）的哈希与 PowerShell `Get-FileHash` 逐字节一致。
   生产代码需要链 `bcrypt`（CMake 加库）。

2. **WinHTTP 二进制下载（Q2）可用**：回环 HttpListener + plain HTTP GET 下载 1204B 一次成功；
   失败路径有界：404 即回（0ms），连接拒绝 `12029`（~2s，连接超时 5s 之内）。
   **Q3 的教训**：`WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` 在本机对 raw.githubusercontent.com 间歇性
   12002 超时（两次复现，curl 同期 200/1.14s 正常），改 `WINHTTP_ACCESS_TYPE_NO_PROXY` 后
   5/5 全过（234-1078ms）。生产主题下载走 NO_PROXY（T-075 天气链路沿用 DEFAULT_PROXY 是既成事实，
   本任务不改动它，但新代码统一 NO_PROXY 并在此记录）。

3. **Shell zip 解包路线被证据否决**（Q4a/Q4c 历次失败记录，最终 spike 里已删除）：
   - `IShellDispatch::NameSpace(<zip>)` 在本控制台进程恒返回 `S_FALSE`/`E_FAIL` + null Folder
     （重试+泵消息+OleInitialize 都不救）；**字节完全相同的同一文件在 PowerShell
     `Shell.Application` 下 NameSpace 成功且 Items()=1** —— 进程上下文相关，不可依赖。
   - `BHID_SFObject` 绑定同一 .zip 的 `GetAttributes(SFGAO_FOLDER)` 返回 0（该进程看不到 zip 为文件夹）。
   - `.dockforge-theme` 扩展名上的 NameSpace 首调 E_FAIL（zip 命名空间按扩展名路由）。
   - 注册表佐证：`HKCR\.zip\shellex` 只有 preview handler（{8895b1c6-...}），**没有** namespace
     handler {000214e6-...}；zipfldr CLSID {E88DCCE0-...} 本身已注册（InprocServer32=zipfldr.dll）。
   - CLSCTX：LOCAL_SERVER 激活 `REGDB_E_CLASSNOTREGISTERED`（0x80040154），CLSID_Shell 只有 in-proc。

4. **miniz 解包（Q4/Q5）可用且确定性**：vendored miniz（master 分支拆分文件集：
   miniz.c/miniz.h/miniz_common.h/miniz_tdef.*/miniz_tinfl.*/miniz_zip.*，本地 `miniz_export.h` 存根
   置 `MINIZ_EXPORT` 空，静态链）。`mz_zip_reader_init_mem` 直接吃下载字节，deflate 解压正常，
   1.2KB 包 0ms、3 文件全部落盘、theme.json 可解析。**选型：miniz**（shell 路线不可依赖；
   miniz 与进程形态无关，也符合 B-003「第三方库就地 vendoring」先例）。

5. **两个包格式兼容坑（生产安装器必须处理，单测将锁定）**：
   - **PowerShell 5.1 `Compress-Archive` 写反斜杠条目名**：`entry[0] name='icons\sample.png'`。
     安装器必须把条目名里的 `\` 归一成 `/`，并逐级创建父目录，否则 `PATH_NOT_FOUND`（0x80070003 实测）。
   - **Compress-Archive 拒绝 `.dockforge-theme` 扩展名**（"不是支持的存档文件格式"，先压 .zip 再改名）。
     安装器读取时不关心扩展名（init_mem 吃字节），此约束只影响造包方。

6. **驱动侧坑（与产品无关，但影响探针）**：HttpListener 必须只在一个进程里持有前缀——
   主进程与 Start-Job 各开一个同前缀 listener 时，主进程接下连接不处理，WinHTTP 端
   12002 超时（实测一次）；残留 HTTP.sys 注册也会卡死固定端口 → 探针用随机端口。
