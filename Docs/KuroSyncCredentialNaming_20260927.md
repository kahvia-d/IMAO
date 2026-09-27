# 缺陷分析：已连接库街区，仍提示「此同步档案尚未连接库街区」

- 玩家数据包：`IMao-WinUI_云英缀玉.zip`（sha256 `25eba7f7…`，302 MB，含 `%LOCALAPPDATA%\IMao-WinUI` 全量数据）
- 涉及账号：库街区 `10146974`，记录本 `local`（显示名「默认」，348 个点位全部已完成）
- 程序版本：`IMao-v2026.9.26.3-windows-x64`（09-27 14:47 +0800 起），此前为 `v2026.9.25.1`
- 分支：`fix/savedpoints-preview-sync`

---

## 1. 结论

**根因是一处命名规则不一致：库街区凭据在磁盘上的文件名，写入方和读取方用的是两套 id。**

| 侧 | 谁 | 用哪个 id 命名 `KuroSync/credentials/<id>.json` |
|---|---|---|
| 写 | 浏览器扩展 `service-worker.js:17-20` | `kuro_` + 库街区账号 → `kuro_10146974` |
| 读 | 桌面端 `KuroProgressSyncService` | 记录本 id → `local` |

玩家记录本的 id 是 `local`（默认记录本），于是：

- 扩展把凭据写进 `KuroSync\credentials\kuro_10146974.json`（文件确实存在，608 字节，DPAPI 加密，`SavedAt = 2026-09-27T07:59:06Z`）；
- 玩家点「预览同步」，桌面端按记录本 id 去读 `credentials\local.json`，文件不存在；
- `KuroProgressSyncService.cs:74` 抛出 `此同步档案尚未连接库街区。请在浏览器扩展中重新连接。`

**截图里那条红字就是这个字符串逐字命中。** 界面另一处同时显示「当前记录本：默认。已绑定库街区账号 10146974」——绑定关系完全正确，缺的只是「按哪个名字找凭据」。

### 为什么这个 bug 长期没被发现

`Docs/LocalAccounts_20260926.md` §4.3 把凭据定义为 `credentials/<ledgerId>.json`，并在 §8 记下一条关键假设：

> 历史玩家的账本 id 恰好就是 `kuro_<账号>`，因此旧凭据文件仍然对得上。

对**老玩家**成立：他们的记录本 id 逐字就是 `kuro_10146974`，两侧字符串碰巧相同，于是同一份文件被找到。
对**任何记录本 id 不等于 `kuro_<账号>` 的玩家**（默认记录本 `local`、`acc_xxxxxxxx` 新建记录本）不成立——两套 id 分叉，凭据永远读不到。

这个 bug 从同步功能第一版 `47beb74` 就存在（`profileId()` 当时就是这样写的），`b3eefbf` 只加了 `accountId` 字段、没动命名，所以玩家说「之前旧版本也存在」是准确的，不是新版引入的回归。

### 为什么「重新连接」修不好

这是本缺陷最恶劣的一点：**提示文案让玩家做的动作，按设计不可能成功。**

1. 文案要求「请在浏览器扩展中重新连接」；
2. 扩展 `profileId()` **无条件**给输入加 `kuro_` 前缀（`service-worker.js:19`）；
3. 所以无论玩家留空（自动识别账号）、还是手动填账号 `10146974`、还是照文档 `KuroMapSyncInstall.md:30` 的「档案 ID 记下来备用」把 `kuro_10146974` 填进去，落盘的名字都不会变成 `local`：
   - 留空 / 填 `10146974` → `kuro_10146974`
   - 填 `kuro_10146974` → `kuro_kuro_10146974`
4. 玩家重复连接任意多次，结果都是「连接成功」+「尚未连接」的死循环。

补一句：`Docs/KuroMapSyncInstall.md:30` 让玩家「把档案 ID 记下来备用」，而全文档没有一处告诉玩家这个 ID 要用在哪里——桌面端从头到尾没有把记录本 id 交给扩展的通道（native messaging 只有扩展→桌面一个方向，`KuroBridgeRegistration.cs` 只注册宿主，不传数据）。

---

## 2. 证据

### 2.1 数据包内的文件互证

| 文件 | 关键内容 | 说明 |
|---|---|---|
| `SavedPoints\accounts.json` | `activeAccountId: "local"`，`[{id: "local", name: "默认", kuroAccountId: "10146974"}]`，`createdAtUtc: null` | 记录本 id 是 `local`；`null` 时间戳说明它是 `Seed()` 播种出来的，不是新建 |
| `SavedPoints\ledger-report.json` | `hasCredential: false`（`local`） | 见 §2.3：这条不能作为独立证据，但暴露了同一个判定规则 |
| `KuroSync\credentials\kuro_10146974.json` | 存在；`AccountId: ""`，`SavedAt 2026-09-27T07:59:06Z` | 凭据**在**，只是名字是 `kuro_`+账号 |
| `KuroSync\credentials\local.json` | **不存在** | 桌面端要找的文件 |
| `SavedPoints\profiles\local.json` | `schemaVersion 2`／`profileId "local"`／`revision 374`／348 点／`syncStates: []` | 进度文档本身完好，348 点全部 `completed/localTouched/pending = true`、`remoteCompleted = null` |
| `KuroSync\device-id.txt` | `897204b1f7984af6bed808fcf45c45ac` | 设备标识已生成 |

### 2.2 截图

- 截图 2（扩展弹窗）：**「已连接到同步档案 kuro_10146974。」** —— 这正是 `popup.js:5` 的 `已连接到同步档案 ${response.profileId}`，`response.profileId` 就是桥接回显的 `request.ProfileId`（`tools/KuroSyncBridge/Program.cs:30`）。**扩展自认连接成功，且名字是 `kuro_10146974`。**
- 截图 1（设置页）：**「当前记录本：默认。已绑定库街区账号 10146974；预览与应用只作用于这一本记录本。」**（对应 `SettingsPage.xaml.cs:914`）—— 绑定正确，`KuroSyncPreviewButton.IsEnabled = active.IsBound` 为真，所以按钮可点。
- 截图 2 红字：**「此同步档案尚未连接库街区，请在浏览器扩展中重新连接。」**（对应 `KuroProgressSyncService.cs:74/136`）。

三处互证：**绑定对、凭据在、就是找不到。**

### 2.3 时间线（`Logs\startup.log`＋`events-20260927.jsonl`＋文件时间戳，UTC）

| 时刻（UTC） | 事件 |
|---|---|
| 09-26 14:01:39 | `profiles\local.json` 最后一次写入（此后未再变动） |
| 09-27 06:47:41 | 程序升级到 `2026.9.26.3` |
| 09-27 07:44:07–07:44:58 | **9 次 CoreHost 崩溃 `invalid-profile-document`**（见 §3.1） |
| 09-27 07:55:07 / 07:58:33 | 又两次启动；本次会话日志到 07:58:40 为止 |
| 09-27 07:56:01 | 玩家点「导出体检」→ 生成 `ledger-report.json` |
| 09-27 07:59:06 | 玩家点扩展「连接桌面端」→ 写入 `credentials\kuro_10146974.json` |
| 截图 | 点「预览同步」→ 红字「尚未连接库街区」 |

**关于体检报告**：`hasCredential: false` 生成于连接之前 3 分钟，所以它单独看只是「当时确实没有凭据」，**不构成独立证据**。但它有意义的地方在于：`LocalAccountCatalog.cs:238-239` 的判定就是 `File.Exists(credentials\<记录本id>.json)`，**即便玩家连接后重新导出，这一行仍然是 `false`。** 这份文件正是我们在 §8 设计里专门用来替代「靠猜」的排障输入，而它对这个故障会给出错误答案——发布前应该一起改掉（§4.2 第 3 条）。

---

## 3. 次要发现

### 3.1 九次 CoreHost 崩溃：`invalid-profile-document`（同一类脆弱性，但不是本次故障的原因）

- 现象：09-27 15:44 +0800 的 50 秒内，`host | corehost-started` 之后紧跟 `host | unhandled-cpp-exception`，共 9 次；每次都留下 `CrashReports\corehost-20260927-*.json`，内容一致：

  ```json
  { "details": "invalid-profile-document", "source": "unhandled-cpp-exception", "timestamp": "2026-09-27T15:44:58+0800" }
  ```

- 崩溃点确定：`CoreHostMain.cpp:864` 记录 `corehost-started` 后才进 `Initi()`（`:866`），而 `Initi()` → `DrawItemBase::Initi()` → `MarkerCompletionStore` 构造函数（`MarkerCompletionStore.h:17` 直接 `document = Load("local")`），`Load` 是**全仓唯一**抛 `invalid-profile-document` 的地方（`MarkerCompletionStore.h:475-476`）：

  ```cpp
  if (result.value("schemaVersion", 0) != 2 || result.value("profileId", "") != profile ||
      !result.at("points").is_object() || !result.at("syncStates").is_array())
      throw std::runtime_error("invalid-profile-document");
  ```

  崩溃的进程一条 core 事件都没打，与「构造函数即抛、来不及跑任何逻辑」完全吻合。
- **但包里的 `profiles\local.json` 四条检查全部通过**（`schemaVersion=2`、`profileId="local"`、`points` 是对象、`syncStates` 是数组），且它自 09-26 14:01:39 起未再改动。也就是说，**触发那 9 次崩溃的文件状态没有留在包里**（可能被玩家后来的替换/复制覆盖掉；文件 mtime 会被复制操作保留，所以从 mtime 无法反推）。→ 这与玩家主诉**无因果关系**（主诉那次 CoreHost 是活的：截图里 348 个已完成点位都读出来了）。
- 但仍值得修：这里的设计与 C# 侧「读不懂就 fail closed、保留原文件、退回默认」相反——**一份读不懂的文档会让整个 CoreHost 起不来**，玩家看到的是「程序打不开」，而不是「这本记录本读不了」。建议按同一条原则改成隔离 + 新建 + 界面报明（§4.5）。

### 3.2 同步从未成功过一次（不是 bug，是背景）

`profiles\local.json`：`syncStates: []`（云端基线一个区域都没建立），348 个点全部 `pending: true` 且 `remoteCompleted: null`。说明这套 348 个完成点**一次都没推送上去过**，与「凭据从来没被桌面端看见」一致。

### 3.3 待确认：`SavedRoutes\profiles\local.json` 与进度文档字节相同

两份文件 SHA256 都是 `E1E03337FC1C8206BE3855726BC6E4C92015A44DB51D2CDFCE33842462DE9464`（各 98768 字节），即路线目录下放了一份**点位进度文档**。

按当前代码，自动路线在 `SavedRoutes\Auto\<记录本id>\<路线id>.json`（`RoutePlanningService.cpp:361` + `RoutePlanStore.h:127-132`），**全仓没有任何代码写 `SavedRoutes\profiles\`**（`scripts\` 下也没有会这样复制的打包/排查脚本）。所以它要么来自更早的版本，要么是玩家自己在排障时手工复制过去的。**建议向玩家问一句「这个包是怎么打出来的、有没有手动复制过文件夹」，先别当 bug 改。**

---

## 4. 修复方案（建议）

### 4.1 一条规则，写在两个地方各一份 → 收成一处

凭据属于**库街区账号**，不属于记录本。记录本通过绑定指向账号，所以：

> **库街区凭据固定存放在 `KuroSync/credentials/kuro_<库街区账号>.json`。**

这条规则扩展已经在执行（`service-worker.js:19`），桥接也在执行（`Program.cs:29` 原样落盘），只有桌面端在读的时候用了记录本 id。把桌面端对齐，**存量玩家零迁移**：老玩家的记录本 id 本来就叫 `kuro_<账号>`，新旧名字是同一个文件。

### 4.2 改动点

1. `IMao-WinUI.Core\KuroSync\KuroTokenVault.cs`
   增加唯一命名权威（扩展那条规则的桌面端副本）：
   ```csharp
   internal static string CredentialId(string accountId) => "kuro_" + accountId;
   ```
   并在注释里写明它与 `BrowserExtensions/KuroMapSync/service-worker.js` 的 `profileId()` 必须逐字一致。
2. `IMao-WinUI\Services\KuroProgressSyncService.cs`
   新增一个入口，`IsConnected` / `PreviewCoreAsync` / `ApplyCoreAsync` / `PushLocalChangeAsync` / `Disconnect` 全部只走它：
   ```csharp
   // 先按记录本 id（历史 kuro_<账号> 记录本、旧凭据），再按绑定账号。
   private bool TryReadCredential(string profileId, out KuroCredential credential) =>
       vault.TryRead(profileId, out credential) ||
       (BoundAccount() is { Length: > 0 } account && vault.TryRead(KuroTokenVault.CredentialId(account), out credential));
   ```
   `EnsureCredentialMatchesBinding`（`:45-52`）保留——新查法命中的凭据天然账号一致，这条从「主校验」退化为“万一”的兜底，无副作用。
3. `IMao-WinUI\Services\LocalAccountCatalog.cs:238-239`
   `WriteDiagnostics` 的 `hasCredential` 用同一条规则（记录本 id 或绑定账号任一存在即 true），否则体检报告继续对这个故障撒谎。
4. `Disconnect(profileId)` 两个候选名字都删，避免玩家「断开」后凭据还留在 `kuro_<账号>.json` 里。
5. 文档：`Docs/LocalAccounts_20260926.md` §4.3 的 `credentials/<ledgerId>.json` 改成 `credentials/kuro_<绑定账号>.json`（并删掉 §8 那条「历史玩家账本 id 恰好是 `kuro_<账号>`」的侥幸假设）。

### 4.3 扩展侧（可延后，属澄清而非修 bug）

- 弹窗输入框标签「库街区账号标识（留空时自动识别）」与实际语义（会被拼成 `kuro_<值>`）应当说明白；`profileId()` 不应给已经带前缀的值再加一次。
- 顺带一个独立的观察：包里的凭据 `AccountId` 是**空字符串**。`extract-main.js:7-8` 从 `localStorage.AKI_MAP_USER_INFO.userId` 取账号，取不到就留空，而扩展并不因此拒绝连接（只要 token 长度 > 8）。于是「留空时自动识别」会静默失败，玩家只好手填——而手填的正是被错拼的那一条路。**如果玩家装的是 09-26 之前的扩展版本，`accountId` 根本不会上报**（`b3eefbf` 才加的），那 §4.2 的兜底就依赖记录本的绑定值——而绑定值是对的（`accounts.json` 里 `kuroAccountId: "10146974"`），兜底成立。两种来源都指向同一个结论，不影响修复。

### 4.4 回归测试（`Tests/ManagedRuntime`）

| 用例 | 断言 |
|---|---|
| 记录本 `local` 绑定 `10146974`，仅存在 `credentials\kuro_10146974.json` | `IsConnected("local") == true`；`PreviewAsync` 不再抛「尚未连接」 |
| 历史记录本 `kuro_12345`（凭据同名） | 仍按记录本 id 命中，行为不变 |
| 记录本绑定 `999`，磁盘上只有 `kuro_10146974.json` | `IsConnected` 为 **false**（只按绑定账号找，**绝不**退化成「随便找一份凭据」） |
| 凭据 `AccountId` 为空（旧扩展） | 仍可用，`BindingMismatch` 视为「账号未知」放行（保持 `:40-41` 现有语义） |
| `WriteDiagnostics` | 上述第一种情形 `hasCredential == true` |
| `Disconnect("local")` | `kuro_10146974.json` 与 `local.json` 都被删除 |

### 4.5 顺带加固：一份读不懂的进度文档不该让 CoreHost 起不来

`MarkerCompletionStore::Load` 现在对任何不合规文档直接 `throw`，而构造函数在启动路径上就调它，于是异常一路冒到 `CoreHostMain.cpp:962`，进程退出、界面显示「无法启动或连接 CoreHost」。同一仓库的 C# 侧（`LocalAccountCatalog.cs:296-304`）对同类问题是明确的 fail closed：**不覆盖原文件、退回默认记录本、把原因报给玩家**。建议 C++ 侧对齐：

1. `Load` 读不懂时，把该文件原样改名到 `profiles\<id>.json.corrupt-<时间戳>`（保留证据，不删）；
2. 以空文档继续（`schemaVersion 2` / 该 profile / 空 `points` / 空 `syncStates`）；
3. 通过一次 `fault` 事件把「记录本 X 的进度文档无法读取，已隔离保留」报到界面。

这样「点位读不到」就退化成一条可见提示 + 一份可回收的文件，而不是整个核心打不开。

---

## 5. 对玩家的回复口径（草案）

> 你的账号其实**连接成功了**，问题是桌面端按「记录本名」找凭据、扩展按「账号」存凭据，你用的又是默认记录本，所以两边对不上名。这不需要你重装或重连——重连多少次都是一样的结果。
> 修复版会按你记录本上绑定的账号来找凭据，你的本地记录（348 个点）和凭据都不用动。
> 另外想请你确认一下：这份数据包是怎么打出来的？有没有手动复制过文件夹？（我们看到 `SavedRoutes\profiles\` 下有一份和点位进度一模一样的文件，想确认来源。）

---

## 6. 四属性模型与随之定下的三条规则（2026-09-27 评审）

### 6.1 模型

记录本只有四个属性：

| # | 属性 | 规则 |
|---|---|---|
| 1 | id | 创建时生成，**永不改变**（也是进度、路线、凭据之间的唯一锚点） |
| 2 | 名称 | 玩家随便改，只用于区分 |
| 3 | 绑定的库街区账号 | 玩家**显式**填写；一个账号最多绑一本记录本 |
| 4 | 点位数据 | 完成状态、`syncStates` 云端基线、outbox |

**凭据不是第五个属性。** 它属于第 3 条指向的那个账号，文件名 `kuro_<账号>.json`，与记录本 id 无关。

默认记录本出厂状态：名称「默认」、id `local`、绑定为空、**可能已经有点位数据**（玩家先玩后连是常态）。
玩家首次连接扩展、拿到账号 ID 后，自己把它填进想要的那一本。

**这条模型取代了原设计"连接即自动绑定到当前账本"的设想。** 依据有二：一是桌面端根本没有从扩展
获得"当前记录本"的通道（native messaging 只有扩展 → 桌面单向，`KuroBridgeRegistration.cs` 只注册宿主）；
二是显式绑定比自动猜更符合"一本记录本 = 一份进度实体"，多账号玩家也不会被动串账。
（本玩家的 `accounts.json` 就是手填的产物：绑定非空而两个时间戳为 `null`，`Seed()` 给 `local`
只能生成空绑定，`TryBind()` 又不写时间戳。）

### 6.2 规则一：凭据按绑定账号读

见 §4.2。**这是本次故障的修法**：记录本 id 只作为兼容的第一顺位（历史记录本的 id 本身就是
`kuro_<账号>`，两种命名落在同一个文件上）。**不允许**退化成"磁盘上随便找一份凭据"——
绑定到 999 的记录本，即使机器上有 `kuro_10146974.json`，也必须判定为"未连接"。

### 6.3 规则二：填账号 ID 必须闭环自检

玩家从扩展弹窗抄一个 8 位数字填进来，抄错一位的代价目前是"点了预览同步才报错"，而且报错文案
（"请在浏览器扩展中重新连接"）会把玩家引向一个**不可能成功**的动作。所以：**保存绑定后立即检查
本机有没有这个账号的凭据，并把结论直接写在界面上**（有 → 已就绪，可以预览同步；没有 → 本机还没连过
这个账号，请去扩展点「连接桌面端」）。

配套：扩展弹窗必须显示**纯数字账号 ID**（现在显示的是 `kuro_10146974`，玩家照抄进"绑定库街区账号"
会被"只能填数字"当场拒绝），并且 `profileId()` 不能再给已经带 `kuro_` 前缀的值再加一次
（否则玩家把弹窗里那个字符串原样填回去会得到 `kuro_kuro_10146974`）。

### 6.4 规则三：改绑允许，但必须提示并重置云端基线

玩家会手滑填错账号，所以**不能禁止改绑**；但也不能让它静默发生。`syncStates` 随记录本走（§4.2），
把一本**已经同步过**的记录本改绑到另一个账号时，新账号没有的点会被判成"云端撤回"而取消本地完成标记——
玩家打过的点会自己消失。

处理：改绑到**不同**账号时弹窗说明 → 玩家确认 → 绑定生效后重置这本记录本的 `syncStates`（逐个区域
`markerSetSyncState { initialized:false, enabled:false }`）→ 下次同步按新账号重新建立基线。
**基线为空时走 `establishing` 分支，不取消任何点位**（`MarkerCompletionStore.h:251`），所以重置是安全的；
代价只是已经上传过的点会再上传一次，而写接口是幂等的。

首次绑定（原绑定为空）不需要重置：同步要求已绑定，空绑定期间不可能存在基线。

### 6.5 本轮实现范围

| 项 | 状态 |
|---|---|
| 规则一（凭据按绑定账号读）+ 体检报告同一规则 | 本轮实现 |
| 规则二（填 ID 闭环自检 + 弹窗纯数字 + `kuro_` 前缀去重） | 本轮实现 |
| 规则三（改绑提示 + 重置基线） | 本轮实现 |
| 文档 §4.3 / §7 / §8 与"差异"清单 | 本轮更新 |
| "连接即自动绑定" | **放弃**（§6.1） |
| §4.5 崩溃隔离加固（C++） | 另一轮 |
| 扩展上架更新（规则二的弹窗部分需重新发版） | 另一轮 |
