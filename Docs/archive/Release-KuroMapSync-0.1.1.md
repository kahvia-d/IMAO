# IMao 库街区同步扩展 0.1.1（手动安装版）

这是库街区点位进度同步扩展的**手动安装包**，用于 Edge 加载项商店审核通过之前。内容与提交商店的版本相同。

- 打包文件：`IMao-KuroMapSync-0.1.1.zip`（同目录附带 `.sha256` 校验文件）
- 安装步骤：[Docs/KuroMapSyncInstall.md](https://github.com/kahvia-d/IMAO/blob/main/Docs/KuroMapSyncInstall.md)
  简述：桌面端 设置 → 库街区点位进度同步 → 「注册/修复浏览器桥接」→ 解压 zip 到固定目录 → `edge://extensions` 打开开发人员模式 → 「加载解压缩的扩展」→ 打开并登录库街区大地图 → 点扩展图标「连接桌面端」。
- 需要 IMao 桌面程序 **2026.9.17.6 或更高**（程序包自带 `KuroSyncBridge.exe`）。
- 手动安装得到的扩展 ID 与商店版完全一致（`ohmikfaeobbffhlhoocklplniobcfdbg`），桌面端不需要额外设置。
- 扩展不做任何网络请求：只在库街区大地图页读取该页面自身的登录会话，经浏览器官方的 Native Messaging 通道交给本机桌面程序，由桌面程序用 Windows DPAPI 加密保存。

已知限制：浏览器会显示「开发人员模式扩展」提示；**不会自动更新**，扩展升级需要重新下载 zip 覆盖目录内容，再到扩展管理页点「重新加载」。商店版过审后建议改从商店安装，扩展 ID 相同，桌面端凭据与同步档案不受影响。
