# x64/Release 的开发残留（归档于 2026-10-02）

这些文件此前堆在 `x64\Release` 里，`ShardMap` 无法给它们分类，于是 `UpdatePublisher shard-map`
在这个目录上会直接失败，而**分片发布正是下一次发布要走的路线**。

发布树（`Build-ReleaseCandidate.ps1` 的输出）从来不含这些内容，所以历次发布都没有受到影响；
它们只是构建输出目录里的历史堆积。

> **清理后 `x64\Release` 仍有 32 个未分类文件，但那不是残留。** 差异是结构性的：
> 该目录含 `IMao-WinUI.Core.deps.json`、`IMao-WinUI.build.appxrecipe`、`KuroMapFeatureBuilder.exe`，
> 以及 `Styles/*.xbf` 与 `Views/**/*.xbf`——发布树把 XAML 编进 `resources.pri`，不带这些分立的 `.xbf`，
> 也不带只在开发期存在的工具与中间清单。
> **结论：分片自检与分片打包只能针对发布候选的 `program/<版本>` 目录，不能针对构建输出目录。**
> 2026-10-02 实测：发布树 1451 个文件 100% 分类通过；`x64\Release` 清理后仍报 32 个未分类，属预期。

## 归档内容（无法再生成，故保留）

| 文件 | 大小 | 说明 |
| --- | ---: | --- |
| `IMao-CoreHost.before-minimap-fix-20260907.exe` | 35,805,696 B | 小地图修复前的原生主机快照（2026-09-07） |
| `IMao-CoreHost.before-sparse.exe` | 36,597,760 B | 稀疏索引改造前的快照（2026-09-09 20:09） |
| `IMao-CoreHost.pending.exe` | 36,598,272 B | 与上者同期的待用快照（2026-09-09 20:46） |
| `IMao-CoreHost.pending.exp` / `.lib` | 2,632 / 5,376 B | 上者对应的导入库与导出文件 |
| `.kuro-bridge/` | — | 同步桥接的构建残留（`out\map-test` 下有同名副本） |

这些是特定历史时刻的二进制，无法由当前源码重新生成，因此归档而非删除。

## 已删除（可再生成）

- `IMao-CoreHost.exp`、`IMao-CoreHost.lib` —— MSVC 链接产物，每次构建重新生成；
- `IMao-WinUI.pdb`、`IMao-WinUI.Core.pdb` —— 调试符号，**发布目录 `Release/` 里仍有一份**，
  运行与发布都不需要构建输出里的这份。

## 之后如何保持

`x64\Release` 是构建输出目录，不是发布树。做分片自检时应当指向发布候选的 `program/<版本>` 目录：

```powershell
& tools\dotnet-sdk-8.0.424\dotnet.exe tools\UpdatePublisher\bin\Release\net8.0\UpdatePublisher.dll `
  shard-map --app-root out/release-candidate-<版本>/program/IMao-v<版本>-windows-x64
```

如果今后还要在 `x64\Release` 里放临时备份，请放进 `out/`（已被忽略）而不是构建输出目录，
否则分片自检会连带把这些文件报成未分类。
