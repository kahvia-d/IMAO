# 攻略白边、默认位置与图片翻页

## 问题与实现

用户截图中的顶部白条不是完整系统标题栏。上次 `HasTitleBar=false` 和 `WS_CAPTION` 检查不能证明非客户区已消失；本次使用实际窗口客户区、窗口外框和 DWM 可见边界复现了 125% DPI 下的 9 像素顶部空隙。

对照记录 `out/guide-window-frame-ablation.log`：仅禁用调整大小仍有 3 像素；扩展标题区仍有 9 像素；只清除样式位仍有 8 像素。生产窗口现在安装自己的 `SetWindowSubclass` 回调，仅处理 `WM_NCCALCSIZE`，让整个窗口成为客户区，其余消息交回原窗口过程；关闭窗口时移除回调。首次打开、移动、调整大小和重新显示都使用同一处理。该处理与 [Microsoft 的 WM_NCCALCSIZE 文档](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccalcsize)所述的扩展客户区方式一致。

攻略每次打开默认放在游戏客户区左侧、垂直居中，保留 16 DIP 边距。原生 `markerGetGameWindowBounds` 从当前运行会话的权威游戏 HWND 和进程 ID 获取物理客户区边界，不使用任意前台窗口；无有效游戏窗口时退回当前显示器工作区左侧。WinUI 将游戏范围与显示器工作区求交，只对窗口宽高及边距进行 DPI 缩放，并适应负坐标显示器和较小窗口。用户仍可拖动点位标题区域移动攻略。

新增 `guidePreviousImageKey` / `guideNextImageKey`，默认 PageUp / PageDown。五项自定义键一起验证、保存和原子发布，旧配置缺少新字段时采用新默认值。使用指南中可改键、禁用、恢复默认，攻略图片下方实时显示当前绑定。

继续使用原有键盘钩子：只有攻略实际可见，且游戏或攻略窗口有焦点时才接管翻页；隐藏时不开始接管，一次物理按下翻一页。`markerGuidePageRequested` 携带窗口句柄、档案、点位和会话代次，协调器核对后才切图。首尾不循环，旧点位事件不影响新攻略。放大图同步翻页及页码，并在加载失败时显示错误。刷新返回零张图片会清理旧图片和取消令牌，避免后续关闭异常。

## 验证

- 常用 `x64/Release/` 完整 Release 构建成功：核心时间 16:29:08，WinUI DLL 时间 16:29:28，0 错误，保留原有可空性警告。6 个页面导航检查通过；资源、大小地图定位资源均就绪。
- 原生优化、标记、路线测试全部通过，托管及真实 CoreHost 通信检查 369 项 PASS。包括五键默认/旧配置/冲突/禁用/持久化/部分更新、翻页事件身份和按压所有权，无运行会话时不返回伪造游戏范围。
- 独立 WinUI 程序 20 项 PASS（1 组坐标计算与 19 个实际窗口场景），0 警告、0 错误。覆盖 100/125/150/200% DPI、负坐标、工作区裁剪、实际左侧定位、窗口位置变化、关闭/重开、翻页首尾/旧事件、零图刷新和放大图失败提示。
- 生产窗口实际测量：外框顶部、客户区顶部、DWM 可见顶部均为 277，顶部间隙 **0 像素**；125% DPI 客户区尺寸 550×825，重开仍为 0。测试程序不再在测试内修改窗口样式或子类化，只检查生产实现。
- Computer Use 对受控两图预览进行截图检查：顶部连续显示深色内容，白条消失；PageUp/PageDown 提示可见；点击下一张从第一张暖色测试图切换到第二张冷色测试图；拖动标题后窗口物理位置从 (100,277) 移至 (200,327)。测试使用本地生成的图片，不连接网络或读取用户进度。

证据：`out/marker-guide-layout-build.log`、`out/marker-guide-layout-tests.log`、`out/marker-guide-layout-runtime/`、`out/guide-window-harness-build.log`、`out/guide-window-runtime/guide-window-tests.log`、`out/guide-window-runtime/guide-window-preview.log`。复现命令见 `Tests/GuideWindowRuntime/README.md`。

未在真实游戏中执行翻页键、完成键或改键，也未修改用户进度。真实游戏的全局键盘路由与多显示器实际切换仍需单独验收，受控窗口和离线回归不计作游戏内验收。
