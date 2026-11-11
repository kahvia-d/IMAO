# 主窗口位置恢复 Implementation Plan

**Goal:** 主窗口记住上次正常关闭时的位置和大小，首次居中，显示器或DPI变化时校正到当前可见工作区。
**Architecture:** 独立用户设置文件保存窗口几何，当前显示器布局为权威；最大化/最小化不覆盖正常还原矩形。删除无效的WinUIEx默认PersistenceId依赖。
**Spec:** 用户已确认上述行为，特别要求避免历史运行时生成文件阻断更新问题。

## Tasks
- [x] 原子持久化至UserDataPaths.Root，验证异常/损坏数据，记录诊断。
- [x] 首次居中、正常关闭恢复、DPI与显示器变化校正；保留正常还原矩形。
- [x] 真实WinUI测试，程序目录前后对比，重跑1fd9414对应imgui.ini及未知文件拒绝回归。
- [x] 真实自包含publish、脚本刷新测试树、逐文件校验与重启。

## 历史问题与边界
1fd9414修复的是ImGui的imgui.ini写入程序目录，被清单校验判为损坏。新位置文件必须放%LOCALAPPDATA%/IMao-WinUI，不能通过扩大RuntimeArtifacts允许名单规避校验。不得修改C:/Dapps/IMao。构建前Enter-DevEnvironment；用户已授权自行关闭核验路径的测试客户端。

## 验证结果
12项纯逻辑、10项真实MainWindow生命周期、62项更新器回归通过。真实WinUI自包含publish通过；脚本刷新396载荷 differing=0。测试树实际启动/正常关闭对照 fileInventoryDifferences=0、programFileHashDifferences=0，位置文件仅在用户数据目录。证据：out/evidence/window-placement-directory-proof.json、window-placement-tree.json、window-placement-update-final/report.json；真实窗口日志在TEMP/IMao-window-placement-runtime.log。本轮未生成新的完整ZIP。

