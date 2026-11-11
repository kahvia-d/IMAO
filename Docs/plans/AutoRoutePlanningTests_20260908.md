# 自动路线规划测试命令

在已完成项目依赖配置的 x64 Visual Studio 开发环境中，从仓库根目录执行：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release-route-planning-tests --parallel 2
.\x64\Release\IMaoRoutePlanningTests.exe
```

该构建预设只构建独立路线测试，不链接 CoreHost。若 `cmake` 不在 PATH，使用 Visual Studio 安装目录下的 `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`。从普通 PowerShell 启动开发工具子进程时，沿用项目脚本合并 `Path`/`PATH` 环境变量的处理。

测试包含固定起点开放路线、目标身份保留、确定性与取消、矩形/圆形线段裁剪、套索几何、Esc 按键所有权、持久化失败与兼容性、档案隔离、跳过撤销恢复。性能检查对 500 个目标运行 20 组不同输入，输出 P95 和最大耗时，P95 门槛为 1000 毫秒。

项目完整运行时检查已包含该目标：

```powershell
.\scripts\Test-Runtime.ps1
# 已有完整构建产物时：
.\scripts\Test-Runtime.ps1 -SkipBuild
```

默认日志为 `out\system-audit\route-planning-tests.log`，可通过 `-OutputDirectory` 修改。`-SkipBuild` 遇到尚未构建的新测试 EXE 时会明确提示跳过，避免破坏使用旧产物的检查流程；需要路线测试结果时先构建上述独立预设。

这些是离线算法、存储与回归检查。真实游戏中的鼠标输入、地图切换及导航显示仍需游戏内验收。

## 2026-09-08 本机最终结果

执行 `scripts/Test-Runtime.ps1 -OutputDirectory out/auto-route-final-runtime -Parallel 2`，退出码为 0。

- 原生自动路线测试（包含 Esc 所有权测试）通过；500 点 × 20 组，P95 **13.7299 ms**，最大 **15.2267 ms**。
- 原生标记测试和现有优化回归通过。
- 托管运行时与真实 CoreHost 管道测试通过，日志中共 **224 项 PASS**。测试使用该输出目录下独立的应用数据，包含预览启用/加载后防止旧草稿覆盖进度、过期上下文拒绝和手动起点重算。
- 完整 Release 构建、5 个 WinUI 页面导航元数据检查、资源探针通过；资源探针的 resourcesReady、viewportReady、visualReady 均为 true。构建日志保留原有数值转换、旧路径 API 及可空引用警告，不影响本次构建与回归通过。

证据文件：

- `out/auto-route-final-build.log`
- `out/auto-route-final-runtime/native-build.log`
- `out/auto-route-final-runtime/native-tests.log`
- `out/auto-route-final-runtime/marker-tests.log`
- `out/auto-route-final-runtime/route-planning-tests.log`
- `out/auto-route-final-runtime/managed-tests.log`

未检测到游戏进程，实际游戏验收和同场景开启前后定位/绘制性能对比尚未完成。
