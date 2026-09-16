# IMao 库街区同步扩展

扩展只在 `kurobbs.com/mc/map` 页面读取库街区地图前端使用的 `AKI_MAP_USER_TOKEN` 和账号 `userId`。点击“连接桌面端”后，token 经 Chrome/Edge Native Messaging 发送到本机 `KuroSyncBridge`，由当前 Windows 用户的 DPAPI 加密保存。扩展不读取 Cookie，不访问其他站点，不在自身存储、页面、控制台或日志中保存 token。

## 本地安装

1. 在 Chrome/Edge 的扩展管理页打开开发者模式，选择“加载已解压的扩展程序”，目录为本目录。
2. 复制扩展管理页显示的 32 位扩展 ID。
3. 在仓库根目录执行 `scripts/Install-KuroSyncBridge.ps1 -ExtensionId <扩展ID>`。该命令将构建桥接程序并仅在当前用户注册 Chrome/Edge Native Messaging Host。
4. 打开已登录的库街区鸣潮大地图，刷新页面，点击扩展图标并选择“连接桌面端”。

连接成功后，凭据位于 `%LOCALAPPDATA%\IMao-WinUI\KuroSync\credentials`，内容只能由同一 Windows 用户解密。删除或卸载扩展不会自动删除桌面凭据；桌面端的“断开”操作应显式删除对应档案。

当前仓库已验证接口地址和认证头：`POST https://api.kurobbs.com/map/core/position/getHaveDonePositionIds` 使用 `token` 请求头读取完成点位。写入完成/取消完成接口的请求体仍需从实际页面操作中记录并加入回归 fixture 后才能开放自动写入。
