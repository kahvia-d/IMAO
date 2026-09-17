# IMao 库街区进度同步扩展 · 隐私说明

最后更新：2026-09-17

## 这个扩展做什么

它只有一个用途：把**你本机 IMao 桌面程序**与你在**库街区鸣潮大地图**（`www.kurobbs.com/mc/map/`）上标记的点位完成状态对应起来。

## 它读取什么

只在该地图页面上读取两项内容：

- `AKI_MAP_USER_TOKEN`：你在库街区地图页面登录后，页面自身存放在浏览器本地的登录凭据。
- `AKI_MAP_USER_INFO.userId`：用于区分不同账号的进度档案。

不读取 Cookie，不读取其他站点的任何数据，不记录键盘输入，不截取页面内容。

## 它把数据发给谁

**只发给本机的 IMao 桌面程序**，通过浏览器官方的 Native Messaging 通道（本机注册表中的宿主清单里只允许本扩展 ID）。数据不经过任何服务器，扩展本身也不做任何网络请求。

桌面端收到凭据后，用当前 Windows 用户的 DPAPI 加密保存在本机 `%LOCALAPPDATA%\IMao-WinUI\KuroSync` 下，用于代替你在网页上读取/写入自己的点位完成状态。凭据不会出现在日志、截图、剪贴板或导出文件里。

## 数据保留与删除

- 凭据只保存在你自己的电脑上，卸载扩展不会自动删除它；在 IMao 设置页的同步区块可以删除对应档案的凭据。
- 扩展自身不保存 token：会话信息只在浏览器内存态存储（`chrome.storage.session`）中短暂存在，交接完成后立即删除。

## 我们不做什么

- 不把任何数据发送到第三方或我们的服务器（我们没有服务器）。
- 不出售、不共享、不用于广告或画像。
- 不读取库街区以外的站点。

## 联系方式

通过本扩展对应的 GitHub 仓库提交 issue 即可。

---

# IMao KuroMap Progress Sync · Privacy Notice

Last updated: 2026-09-17

This extension has a single purpose: connecting your local **IMao desktop application** with the point-completion state you marked on the **Kuro BBS Wuthering Waves map** (`www.kurobbs.com/mc/map/`).

**What it reads.** Only on that map page: `AKI_MAP_USER_TOKEN` (the session token the page itself stores locally after you sign in) and `AKI_MAP_USER_INFO.userId` (to tell accounts apart). No cookies, no other sites, no page content.

**Where the data goes.** Only to the local IMao desktop application over the browser's Native Messaging channel. Nothing is sent to any server; the extension performs no network requests of its own. The desktop app stores the credential encrypted with Windows DPAPI under `%LOCALAPPDATA%\IMao-WinUI\KuroSync` for the current Windows user.

**Retention.** The credential stays on your machine only. Removing the extension does not delete it; you can delete it from the IMao settings page. The extension itself keeps the token only in session memory and clears it right after the handoff.

**We never** sell or share your data, use it for advertising or profiling, or read any site other than the Kuro BBS map.
