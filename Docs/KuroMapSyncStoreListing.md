# Edge 插件上架材料（Partner Center 填报用）

> 用途：Microsoft Edge Add-ons 提交时逐项复制。所有内容都按当前实现如实描述，不夸大。

## Partner Center 登记信息

（2026-09-17 已提交首次发布申请）

| 字段 | 值 | 说明 |
| --- | --- | --- |
| CRX ID | `ohmikfaeobbffhlhoocklplniobcfdbg` | **浏览器用的扩展 ID**；桥接 `allowed_origins` 填它。由上架用的 public key 推导得出（SHA-256 前 16 字节按 a–p 映射），已写进 manifest 的 `key` |
| Store ID | `0RDCKC4S0B0X` | 商店侧内部标识，浏览器不使用 |
| Product ID | `f7066515-82f5-4dd9-b530-b1a2d0a3a017` | Edge Add-ons 发布 API 的 `products/{productID}` 路径用它 |
| Public key | 已写入 `manifest.json` 的 `key`（392 字符 DER base64） | 让解压加载的开发版与商店版共用同一个 CRX ID |

## 基本信息

- **名称**：IMao 库街区进度同步
- **简短描述（≤ 132 字符）**：把库街区鸣潮大地图的点位完成状态与你本机的 IMao 桌面程序对应起来，双向补齐，不需要上传到任何服务器。
- **分类**：生产力 / 效率（Productivity）
- **语言**：中文（简体）；可另加 English
- **隐私政策 URL**：需指向 `Docs/KuroMapSyncPrivacy.md` 的公开链接（可先用仓库 raw 链接，后续换成 GitHub Pages 页面）
- **支持链接**：仓库 issue 页

## 详细描述（草稿）

这个扩展只有一个用途：让你本机的 IMao 桌面程序知道你库街区账号的点位完成状态，并把两边互相补齐。

工作方式：

1. 你在浏览器里登录库街区鸣潮大地图。
2. 点扩展图标 → 连接桌面端。扩展把当前页面的登录会话通过浏览器官方 Native Messaging 通道交给本机 IMao（不经过任何服务器）。
3. 回到 IMao 桌面程序的「设置 → 库街区点位进度同步」，先「预览同步」看两边差异，再「应用同步」：库街区已完成的会拉进本地，本地已完成的会写回库街区。

特点：

- 只读取该地图页面自身存放的登录凭据与账号 ID，不读 Cookie，不访问其他站点。
- 不做任何网络请求，数据只发往本机桌面程序，由桌面程序在你自己电脑上加密保存。
- 同步只做并集，两边只会互相补齐，不会取消任何点位。

使用前提：需要安装并运行 IMao 桌面程序（Windows x64）。

## 权限理由（逐项填写）

- **nativeMessaging**：扩展需要把库街区会话交给本机 IMao 桌面程序。这是浏览器提供的唯一官方通道，且只能发给本机注册过的宿主；扩展本身不做网络请求。
- **storage**：只在浏览器会话内存中短暂保存待交接的会话信息（`chrome.storage.session`），交接完成后立即删除。
- **站点访问权限（`https://www.kurobbs.com/*`、`https://kurobbs.com/*`）**：只用于在该地图页读取页面自身存放的登录凭据与账号 ID。不使用 `tabs`、不申请 `<all_urls>`。
- **远程代码**：不使用。扩展不加载、不执行任何远程代码或远程脚本。

## 数据用途声明（Data usage / 认证项）

- 处理的数据类型：**身份验证信息**（库街区地图页面的会话 token）。
- 用途：仅用于在你本机识别你的库街区账号，以读取并写回你自己账号的点位完成状态。
- 传输目标：仅本机 IMao 桌面程序（Native Messaging）。不传输给任何第三方，不上传服务器。
- 保存：由桌面程序以 Windows DPAPI 加密保存在当前 Windows 用户目录下；扩展自身不持久化 token。
- 不出售数据，不用于广告、画像或与扩展用途无关的目的。

## 认证测试说明（Certification testing notes，草稿）

审核要点说明：

1. 本扩展必须与本机 IMao 桌面程序配合使用，单独安装无法连接——这属于设计预期，界面会明确提示「请先安装并运行 IMao 桌面程序」。
2. 复现路径：打开 `https://www.kurobbs.com/mc/map/`（需登录）→ 点击扩展图标 → 若桌面程序未安装或未注册 Native Messaging 宿主，会提示「找不到宿主 / 未找到可用登录会话」；这是预期的错误路径。
3. 扩展不发起任何网络请求，因此不需要测试账号即可验证「无外发流量」；如需验证读取行为，任意登录状态的库街区地图页均可。
4. 扩展不含远程代码，权限仅 `nativeMessaging` + `storage` 与两个 kurobbs 域名。

## 提交前检查清单

- [x] 图标：16/32/48/128 PNG（已放在 `BrowserExtensions/KuroMapSync/icons/`）
- [x] 权限收紧：已移除 `tabs`
- [x] 首次提交（2026-09-17 已提交）
- [x] **已上架**：https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg
      商店版与手动版共用同一个 CRX ID，桌面端桥接白名单与同步档案都不受影响。
- [x] 隐私政策链接与商店截图（随上架材料一并提交，**仓库内没有留存副本**；改版时需与商店页核对）
- [x] 桌面端新版本发布（含同步功能，且能自动注册桥接）
- [x] 桥接白名单 ID（CRX ID 已获取，已写进桌面端注册逻辑）
