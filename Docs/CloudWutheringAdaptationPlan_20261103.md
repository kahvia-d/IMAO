# 云鸣潮网页版适配实现计划与实现记录

基线：2026-11-03 获取的 `origin/main`，`46030e9b84a032581f540fbc24e5194774c7634b`。分支：`codex/cloud-wuthering-adaptation`。

## 用户确认范围

支持 Windows Chrome、Edge 的云鸣潮网页，无论普通窗口、网页全屏或浏览器全屏。玩家只选择浏览器窗口，游戏区域由 IMAO 自动处理；不提供手动框选、不要求全屏。保留本地游戏客户端模式。完成后发布自包含开发测试载荷，刷新 `C:\Dcode\WWMAP-TOOLS\out\map-test`，不修改玩家安装。

## 实现

- 首页增加“选择游戏窗口”，枚举可见 Chrome/Edge 窗口并提供自动裁剪的 WGC 预览；可以切回本地客户端。选择只在当前进程内保存。
- IPC `captureTarget` 版本 1 传递十进制字符串 HWND、PID、进程创建时间。原生侧复查进程类型、窗口身份、标题和可见性，不信任托管侧输入；运行时拒绝替换为不同目标。
- 浏览器来源强制 WGC，不回退 BitBlt；原生本地来源保持原来行为。
- UI Automation 读取可见网页 Document 和内部大 Image 的物理像素区域，优先图像边界，不读取 URL、账户或登录态。约每 250 ms 重新查询；窗口缩放、DPI 和全屏变化触发重新获取。
- 对完整视频表面检测成对黑边，不固定 16:9。三个不同帧给出相同边界后更新；全黑/暗场不构造新边界。不对探索场景启用稀疏回读，以保留自动边界检测所需的完整画面。
- 所有捕获、HUD 尺寸、叠加位置、鼠标命中、手绘路线和手柄圆环几何使用统一游戏视口。保留 IPC 原客户区字段，新增 `gameViewport` 供攻略窗使用。
- 几何变化增加 revision，撤销旧捕获/叠加、暂停定位并重置地图；异步视觉结果携带原始 revision，不把旧结果绑定到新区域。最小化或切到其他标题的标签时隐藏输出，恢复后重新识别；窗口关闭停止运行。

主要文件：`Runtime/GameCaptureTarget.h`、`BrowserSurface.cpp`、`BrowserContentArea.h`、`BrowserCapture.cpp`、`App.cpp`、`CoreHostMain.cpp`、`CaptureWindowDialog.cs`、`BrowserWindows.cs`、`CoreHostService.cs`。

## 门禁与交付步骤

1. 原生目标身份、十进制解析、视口边界、负坐标屏幕映射、成对黑边、暗帧和几何代次回归。
2. 同尺寸新视口拒绝旧异步证据；原有定位连续性、叠加及手柄回归。
3. 托管目标序列化和运行时服务回归，真 WinUI 构建。
4. 产品 CoreHost IPC 预览冒烟；真实游戏精度测试必须与采样成功分开报告。
5. self-contained `dotnet publish`，验证 `resources.pri`、`includedFrameworks`，用 `Refresh-MapTestBinaries.ps1` 整体刷新已退出的测试树、核对 `differing=0`、重新启动。

## 验证边界

真实 Edge 普通窗口已验证 WGC 捕获与自动排除浏览器栏，详见[捕获记录](CloudWutheringCaptureValidation_20261103.md)。Chrome、不同 DPI、实际探索/大地图和全屏来回切换仍需真实游戏测试；实现支持不等于该矩阵已经全部实测。

UIA 大图像/Document 和黑边是几何证据，不是直播认证。相同标题的伪装标签、静态游戏截图、云端暂停但桌面持续出帧，不能仅凭标题/WGC 时间戳证明可靠辨别。已有 HUD 和定位证据继续控制位置输出。自动黑边识别对非黑色装饰边框和复杂网页布局不作未验证的准确性保证；异常应通过采样修正自动策略，不能要求玩家框选。

没有新增浏览器扩展、自动登录/排队、网页操作或模型训练。超过 100 MB 的下载须另行确认；本次复用本机依赖。
