# IMao 库街区同步扩展

把库街区鸣潮大地图页面的登录会话交给本机 IMao 桌面程序，用于双向补齐点位完成状态。

扩展只在 `kurobbs.com/mc/map` 页面读取该页面自身存放的 `AKI_MAP_USER_TOKEN` 和账号 `userId`。点击「连接桌面端」后，token 经浏览器官方的 Native Messaging 通道发送到本机 `KuroSyncBridge`，由当前 Windows 用户的 DPAPI 加密保存。扩展不读取 Cookie，不访问其他站点，不做任何网络请求，也不在自身存储、页面、控制台或日志中保存 token。

## 玩家安装（商店审核通过前）

完整步骤见仓库的 [Docs/KuroMapSyncInstall.md](../../Docs/KuroMapSyncInstall.md)，摘要：

1. IMao → 设置 → 库街区点位进度同步 → 「注册/修复浏览器桥接」。
2. 下载本扩展的 zip（发行版 `ext-v*`）并解压到固定目录，例如 `%LOCALAPPDATA%\IMao-WinUI\KuroMapSync\`。
3. `edge://extensions`（Chrome 为 `chrome://extensions`）→ 开发人员模式 → 「加载解压缩的扩展」→ 选中该目录。
4. 打开并登录库街区大地图 → 点扩展图标 → 「连接桌面端」。
5. 回到 IMao → 「预览同步」→「应用同步」。

`manifest.json` 里的 `key` 固定了发布公钥，所以手动加载得到的扩展 ID 与商店版一致（`ohmikfaeobbffhlhoocklplniobcfdbg`），桌面端注册的桥接白名单无需改动。

## 开发与手工安装

1. 在 Chrome/Edge 的扩展管理页打开开发人员模式，选择「加载解压缩的扩展」，目录为本目录。
2. 确认扩展管理页显示的 32 位扩展 ID 为 `ohmikfaeobbffhlhoocklplniobcfdbg`（由 `manifest.json` 的 `key` 推导）。
3. 桌面端设置页点「注册/修复浏览器桥接」；只有在没有桌面程序时，才用仓库根目录的 `scripts/Install-KuroSyncBridge.ps1 -ExtensionId <扩展ID>` 手工注册 Native Messaging 宿主。
4. 打开已登录的库街区鸣潮大地图，刷新页面，点击扩展图标并选择「连接桌面端」。

连接成功后，凭据位于 `%LOCALAPPDATA%\IMao-WinUI\KuroSync\credentials`，内容只能由同一 Windows 用户解密。删除或卸载扩展不会自动删除桌面凭据；桌面端的「断开」操作会显式删除对应档案。

接口状态：读取完成点位（`POST https://api.kurobbs.com/map/core/position/getHaveDonePositionIds`，`token` 请求头）与写入完成/取消完成接口均已在开发中实测通过，写入幂等可安全重试。仍未验证的边界：批量上传（只实测单点往返）、多标签页同时连接，以及真实浏览器与游戏内的完整交互验收；详见 `Docs/Release-2026.9.17.1.md` 与 `Docs/KuroProgressSyncPlan_20260916.md`。
