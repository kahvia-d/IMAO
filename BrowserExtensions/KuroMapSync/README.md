# IMao 库街区同步扩展

把库街区鸣潮大地图页面的登录会话交给本机 IMao 桌面程序，用于双向补齐点位完成状态。

扩展只在 `kurobbs.com/mc/map` 页面读取该页面自身存放的 `AKI_MAP_USER_TOKEN` 和账号 `userId`。点击「连接桌面端」后，token 经浏览器官方的 Native Messaging 通道发送到本机 `KuroSyncBridge`，由当前 Windows 用户的 DPAPI 加密保存。扩展不读取 Cookie，不访问其他站点，不做任何网络请求，也不在自身存储、页面、控制台或日志中保存 token。

## 玩家安装

- [Chrome 应用商店](https://chromewebstore.google.com/detail/imao-%E5%BA%93%E8%A1%97%E5%8C%BA%E8%BF%9B%E5%BA%A6%E5%90%8C%E6%AD%A5/elhbnijeefmmmempajadehknbckaopge)
- [Edge 加载项商店](https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg)

1. 使用当前 IMao，在设置中点击「注册/修复浏览器桥接」。旧注册可能未包含 Chrome 商店 ID。
2. 打开并登录库街区鸣潮大地图，点击扩展的「连接桌面端」，记下账号 ID。
3. 在 IMao 的本地点位记录本设置中绑定该账号，再预览并应用同步。

Chrome 商店 ID 为 `elhbnijeefmmmempajadehknbckaopge`；Edge 商店和仓库解压加载版为 `ohmikfaeobbffhlhoocklplniobcfdbg`。当前桌面端同时允许二者，不需要手改白名单。

手动加载和完整排查见[安装说明](../../Docs/KuroMapSyncInstall.md)。

## 开发与手工安装

1. 在 Chrome/Edge 的扩展管理页打开开发人员模式，选择「加载解压缩的扩展」，目录为本目录。
2. 确认扩展管理页显示的 32 位扩展 ID 为 `ohmikfaeobbffhlhoocklplniobcfdbg`（由 `manifest.json` 的 `key` 推导）。
3. 桌面端设置页点「注册/修复浏览器桥接」；只有在没有桌面程序时，才用仓库根目录的 `scripts/Install-KuroSyncBridge.ps1`（默认允许 Chrome 商店及 Edge/手动版两个 ID） 手工注册 Native Messaging 宿主。
4. 打开已登录的库街区鸣潮大地图，刷新页面，点击扩展图标并选择「连接桌面端」。

连接成功后，凭据位于 `%LOCALAPPDATA%\IMao-WinUI\KuroSync\credentials`，内容只能由同一 Windows 用户解密。删除或卸载扩展不会自动删除桌面凭据；桌面端的「断开」操作会显式删除对应档案。

接口状态：读取完成点位（`POST https://api.kurobbs.com/map/core/position/getHaveDonePositionIds`，`token` 请求头）与写入完成/取消完成接口均已在开发中实测通过，写入幂等可安全重试。仍未验证的边界：批量上传（只实测单点往返）、多标签页同时连接，以及真实浏览器与游戏内的完整交互验收；详见 [历史发行说明](../../Docs/archive/Release-2026.9.17.1.md)与[历史同步计划](../../Docs/archive/KuroProgressSyncPlan_20260916.md)。
