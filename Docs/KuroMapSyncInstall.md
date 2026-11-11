# 库街区进度同步扩展：安装说明

## 商店安装（Chrome / Edge，推荐）

| 浏览器 | 商店入口 | 扩展 ID |
|---|---|---|
| Chrome | [Chrome 应用商店](https://chromewebstore.google.com/detail/imao-%E5%BA%93%E8%A1%97%E5%8C%BA%E8%BF%9B%E5%BA%A6%E5%90%8C%E6%AD%A5/elhbnijeefmmmempajadehknbckaopge) | `elhbnijeefmmmempajadehknbckaopge` |
| Edge | [Edge 加载项商店](https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg) | `ohmikfaeobbffhlhoocklplniobcfdbg` |

两种商店安装均无需开发人员模式，并由各自商店更新。Chrome 商店上架链接由维护者于 2026-11-12 提供；本次没有重新核验商店版本号或审核后台。

当前桌面端已同时支持两种商店版。旧版 IMao 或旧桥接注册文件可能只允许 Edge ID：先更新桌面程序，再在设置中点击「注册/修复浏览器桥接」，然后重试连接。

安装商店版后，直接执行下方步骤 1、5、6、7；步骤 2–4 仅用于手动加载。

## 手动安装（开发或不使用商店）

仓库及 GitHub 手动包的 `manifest.json` 固定了 Edge 发布公钥，解压加载的 ID 为 `ohmikfaeobbffhlhoocklplniobcfdbg`，即使在 Chrome 中手动加载也如此；它与 Chrome 商店 ID 不同。不要为匹配 Chrome 商店 ID 而自行修改 manifest 的 key。

## 同步行为核对（2026-11-12）

下文按现有桌面端 `KuroProgressSyncService`、`MarkerCompletionStore::DesiredCompletion` 与 `KuroSyncSchedule` 更新；旧版“永远只做并集、不会取消”的说明已不适用。此处只核对仓库实现，没有对线上账号执行同步。

## 前提

- 使用当前正式版 IMao 的完整程序包，其中应包含 `KuroSyncBridge.exe`。
- Edge 或 Chrome（Windows）。两者步骤相同，只是扩展管理页地址不同。

## 步骤

1. **注册桥接**：打开 IMao → **设置 → 库街区点位进度同步** → 点「注册/修复浏览器桥接」。
   确认状态行显示桥接已注册。若提示找不到 `KuroSyncBridge.exe`，说明用的不是完整程序包，请重新下载完整包。
2. **下载扩展**：从 https://github.com/kahvia-d/IMAO/releases/tag/ext-v0.1.1 下载 `IMao-KuroMapSync-0.1.1.zip`。
3. **解压到固定目录**：例如 `%LOCALAPPDATA%\IMao-WinUI\KuroMapSync\`。
   不要解压到「下载」文件夹——清理后就失效了。解压完成后，该目录下应当**直接**能看到 `manifest.json`。
4. **加载扩展**：地址栏打开 `edge://extensions`（Chrome 是 `chrome://extensions`）→ 打开「开发人员模式」→ 点「加载解压缩的扩展」→ 选中第 3 步的目录。
5. **连接**：打开并登录 https://www.kurobbs.com/mc/map/ → 刷新页面 → 点扩展图标 → 「连接桌面端」。
   提示「已连接库街区账号 …」即成功；**把这个账号 ID 记下来，下一步要用**。
6. **绑定记录本**：回到 IMao → 设置 → 本地点位记录本 → 选中要同步的那一本 → 点「修改」→
   把账号 ID 填进「绑定库街区账号」→ 保存（粘贴扩展里那串 `kuro_数字` 也行，前缀会自动去掉；存下来的始终是纯数字）。
   保存后界面上会直接告诉你这台机器有没有这个账号的凭据
   （有 → 可以同步了；没有 → 回去点扩展的「连接桌面端」）。
   > 记录本用的是哪一本、账号填在哪一本上，是玩家自己的选择：IMao 不会替你挑，也不会在你连接扩展时自动绑定。
7. **同步**：回到 IMao → 设置 → 库街区点位进度同步 → 先「预览同步」看两边差异 → 再「应用同步」。
   首次建立同步基线时合并两边的完成点。建立基线后，本机明确标记完成或取消的变更都可上传；库街区把曾经同步为完成的点取消时，应用同步也可取消本地对应状态。库街区从未记录过的本地完成点会保留并排队上传，不因云端缺少该点就直接清除。预览中应同时检查拉取、待上传和将取消的数量；预览后云端发生变化时需重新预览。
   打开「自动同步」后，本地完成/取消变更会触发即时推送，另每 10 分钟检查一次；失败按退避策略重试，不承诺每次都即时成功。
   > 给一本**已经同步过**的记录本换绑另一个账号时，IMao 会先确认再重置这本记录本的云端基线：
   > 本地点位一个都不会少，已经上传过的点会按新账号再传一次（写入接口幂等）。

## 手动加载的限制

- 浏览器会显示「开发人员模式扩展」提示；Edge/Chrome 重启后可能提示「停用开发人员模式扩展」，选择保留即可。
- **不会自动更新**：扩展更新时需要重新下载 zip、覆盖目录内容，再到扩展管理页点「重新加载」。
- 改用商店版时先移除手动加载版，避免同时连接两份扩展。Chrome 商店 ID 与手动版不同，安装后重新注册/修复桥接并连接账号；本地记录本不会因卸载扩展而删除。

## 常见问题

- **检查扩展 ID**：Chrome 商店版为 `elhbn…`，Edge 商店版和仓库手动版为 `ohmik…`；这两种都是受支持的 ID。手动加载时选择直接包含 `manifest.json` 的目录。
- **点「连接桌面端」提示找不到宿主**：桥接没注册（回到步骤 1），或者浏览器与桌面程序不在同一个 Windows 用户下。
- **提示「未找到可用登录会话」**：先在库街区地图页完成登录，再刷新页面重试。
- **换电脑或换浏览器**：安装对应商店版，重新注册桥接并登录连接。记录本可单独迁移，但 DPAPI 凭据绑定原 Windows 用户环境，不能靠复制文件保证可用；新机器应重新连接账号。

## 数据与隐私

扩展只读取库街区地图页面自身存放的登录凭据与账号 ID，不做任何网络请求，数据只通过浏览器官方的 Native Messaging 通道交给本机桌面程序，由桌面程序以 Windows DPAPI 加密保存在当前 Windows 用户下。完整说明见 [KuroMapSyncPrivacy.md](KuroMapSyncPrivacy.md)；商店上架材料见 [KuroMapSyncStoreListing.md](guides/KuroMapSyncStoreListing.md)。
