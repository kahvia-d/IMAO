# 离线导入入口修复记录（2026-09-09）

程序版本：`2026.9.9.2`。本次修复文件选择入口，不变更地图内容或用户数据格式。源码修改尚未提交，交付仍为本地验收构建，未推送或发布线上更新。

## 问题与修复

首版点击“导入离线包”时，`Marshal.SizeOf<OpenFileName>()` 报出 `cannot be marshaled as an unmanaged structure`，尚未打开文件窗口或读取 ZIP。原因是 `OPENFILENAMEW` 的输出缓冲区被声明成结构体中的 `StringBuilder` 字段。已改成显式的原生指针，分配 32768 个 UTF-16 字符的可写缓冲，初始化终止符，并在选择、取消和异常路径统一释放。结构数值字段与 Windows 的 DWORD/WORD 对齐，ZIP 过滤器显式以双 NUL 结束。

真实系统对话框测试还复现取消后工作目录从仓库变成“文档”目录。Windows 文档说明 `OFN_NOCHANGEDIR` 对 `GetOpenFileName` 无效；现已在退出对话框时显式恢复原工作目录。参考 [OPENFILENAMEW 定义](https://learn.microsoft.com/en-us/windows/win32/api/commdlg/ns-commdlg-openfilenamew) 和 [.NET 原生互操作说明](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/best-practices)。

之前的更新验收直接传入 ZIP 路径，漏掉实际文件选择入口。本次独立回归既链接生产源码，也支持加载实际交付的 `IMao-WinUI.dll`，已加入 `Test-Runtime.ps1` 和 `Test-ProgramReleasePackage.ps1`，未来验包必须经过此入口检查。

## 验证

| 检查 | 结果及证据 |
|---|---|
| 旧源码失败复现 | 精确复现同一 `Marshal.SizeOf` 异常；`out/resource-picker-regression/before/tests.log` |
| 实际交付 DLL 与系统对话框 | 33/33 通过：x64 结构大小、字段偏移、缓冲类型；取消、选择中文及空格 ZIP 路径、再次取消、工作目录恢复；`out/resource-picker-regression/delivery-final/picker-regression.json` |
| 集成验包入口 | 资源预检、最小 PATH 下 CoreHost IPC 与快照标识、正常退出、实际交付 DLL 文件选择器、包内容不变；`out/picker-fix/integrated-package-verification/package-probe.json` |
| 实际原离线 ZIP 配对 | 完整离线校验与安装、重启启用、真实 CoreHost 就绪确认、整套回退、零网络请求和用户数据哨兵逐字节不变；`out/resource-real-e2e-picker-fixed/real-e2e.json` |
| 自包含 WinUI 构建 | 成功；保留既有 StringItems 空值分析警告；`out/picker-fix/publish.log` |

对话框测试在独立测试进程中运行，只操作该进程自己的窗口；未控制正在使用的程序。验包和资源启用测试使用仓库 `out` 内独立数据目录。沙箱曾拒绝本机命名管道连接，保留失败输出，经自动审批后在沙箱外通过。未重新进行实机游戏定位、实体手柄或完整管理员 WinUI 页面手动验收；原有地图回放结果见首版报告。

## 交付及试用

完整程序包：`out/update-delivery-picker-fixed-2026.9.9.2/IMao-v2026.9.9.2-windows-x64.zip`，526,289,256 字节，1166 个文件。

SHA-256：`fc2cd59117968b816f70fda88b0e788409dcdc0e6f0db822b667916f213eb9b4`。

继续使用原签名离线资源包：`out/resource-release-2026.9.9.1-final/resources-2026.9.9.1-offline.zip`。

SHA-256：`c409e287615d380909ad5883e26b1bdbb54a825194fbc295ed5e4607e767517e`。

退出旧程序，将新完整包解压到新目录，运行 `IMao-WinUI.exe`，确认程序版本为 `2026.9.9.2`。在设置中再次导入原离线 ZIP，等提示后完整退出并重开，资源标识应为 `resources-2026.9.9.1`。本次新包自带资源标识为 `bundled-2026.9.9.2`，测试整套回退后恢复此标识。程序和资源版本独立，不要求数字一致。

保留旧目录及本地记录，勿通过清空 `%LOCALAPPDATA%\IMao-WinUI` 处理该文件窗口问题。
