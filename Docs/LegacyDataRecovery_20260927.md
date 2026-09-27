# 旧版本地数据修复（帮助页）

- 用户需求（2026-09-27）：停留在旧版本的玩家，其本地点位数据在新版本里用不上；希望在**帮助页最下面**有一个「旧版本地数据修复」栏目，说明什么算旧数据，并提供一个「数据恢复」按钮，自动把这些「非记录本形式的旧数据」构造成一本新记录本。
- 实现：`IMao-WinUI/Services/LegacyPointRecovery.cs`（扫描 + 恢复 + 日志）、`IMao-WinUI/Views/UsageGuidePage.xaml(.cs)`（栏目与按钮）。
- 分支：`fix/savedpoints-preview-sync`

---

## 1. 历史考证：旧版本到底把点位数据放在哪

按 git 历史逐个 tag 核对（`git show <tag>:IMao-Core/src/Runtime/MarkerCompletionStore.h`）：

| 时代 | 位置 | 格式 | 今天能读吗 |
|---|---|---|---|
| 重写前（`e4ad9d4 first`、`1a8751c` "Update 10.5"，即**没有记录本、也没有云同步**的那一版及其之前） | **程序目录**下 `<程序目录>\SavedPoints\account_1.json`（`DrawItemBase::SaveItemPoint` 写入，全部点位挤在**一个文件**里） | `{"<场景>": {"<nameId>": [{"id": "<pointId>"}, …]}}` | 文件格式能解析，但**多半找不到**（见 §2） |
| 账号时代（`d363414` 起，含 **v2026.09.09 / v2026.9.9.3 / .9.4 / .9.6**、9.17.x、9.18.x…） | `%LOCALAPPDATA%\IMao-WinUI\SavedPoints\profiles\<id>.json`，另有 `account_1.json` 的副本与 `kuromap-accounts.json` 选档 | `schemaVersion: 2` + `points` + `syncStates` | **能读**：`schemaVersion`／`syncStates`／`profiles/` 三者全部由 `d363414` 一次引入，并且**存在于 v2026.09.09 之后的每一个 tag**，与今天的文档格式完全一致 |
| 云同步时代（9.17.x 起） | 上面那些 + `KuroSync\credentials\kuro_<账号>.json` | — | 凭据命名问题另见 `Docs/KuroSyncCredentialNaming_20260927.md`（已修） |

**结论**：不存在"新版读不懂的旧文档格式"——从 v2026.09.09 起文档格式就没变过；`MarkerCompletionStore::Load()` 的四条校验（`schemaVersion==2`、`profileId` 相符、`points` 是对象、`syncStates` 是数组）在所有 tag 里一字不差。
玩家受困的原因是**位置**与**登记**，不是格式。所以"数据恢复"要解决的是"到不了"，不是"读不懂"。

## 2. 三类真正会把人困住的旧数据

| 类别 | 为什么到不了新版 |
|---|---|
| **旧版单文件** `account_1.json` | ① `DrawItemBase::Initi()` 只在 `profiles/local.json` **不存在**时才把程序目录里的 `account_1.json` 复制进用户目录，`Load("local")` 也只在新建 `local` 文档时才导入它——只要玩家活动账本是别的、或 `local` 已有文档，这个文件**永远不会被读**；② 它只查**当前**程序目录（`GetCurrentPath()`），玩家换了安装目录（`Downloads\IMao-v2026.9.9.6\` → `…\IMao-v2026.9.27.2\`）之后，旧目录里那份**再也找不到**；③ 界面上的「导入旧记录」按钮也只在**用户目录**里存在该文件时才出现。 |
| **没有登记进记录本的进度文档** `profiles/<id>.json` | `LocalAccountCatalog.Seed()` 只在 `accounts.json` **不存在**时播种一次。此后出现在 `profiles/` 里的文档（玩家拷进来的、从 `deleted\` 恢复的、在播种之后才由旧版本写下的）在界面上**没有入口**——文件是好的，但列表里没有它。 |
| **被「删除」移走的记录本** `SavedPoints\deleted\<时间>-<id>\` | 删除是软删除（进度与路线被移进去、不抹掉），但没有任何入口能把它取回来——只能手动搬回去。 |

`kuromap-accounts.json` 的 `ActiveProfile` 只用于播种时预选，本身不含点位，不需要"恢复"，但会在报告里被承认。

## 3. 恢复行为

扫描四处（**全部只读**）：

1. `<用户目录>\SavedPoints\account_1.json`
2. **程序目录及其同级目录**里的 `SavedPoints\account_1.json`（覆盖"换了安装目录"；只扫父目录下一层，最多 200 个目录，避免在大盘上耗时）
3. `<用户目录>\SavedPoints\profiles\*.json` 中**未登记**进记录本的
4. `<用户目录>\SavedPoints\deleted\<时间>-<id>\*.json`

恢复动作按类别区分，**永不修改或删除任何来源文件**：

| 类别 | 动作 |
|---|---|
| 未登记的进度文档 | `LocalAccountCatalog.TryAdopt()` **原地登记**（不复制、不改字节、不切换当前记录本）；id 形如 `kuro_<账号>` 的沿用它的绑定，名字用「库街区 <账号>」 |
| `deleted\` 里的文档 | 复制回 `profiles\<id>.json`（该 id 仍被占用时改用 `legacy_<8位>`，名字相应变成「旧数据恢复（<原 id>）」），再登记；`deleted\` 里的副本保留 |
| 旧版单文件（可能多处） | 合并去重（按 `stateId:pointId`）后**新建一本记录本**「旧数据恢复」，写入文档 |

**扫描结果是可勾选的列表，「数据恢复」只处理勾选的条目**（`Recover(catalog, selected)`）。一台机器上可能存着玩家已经不想再要的某个账号的记录本，"全部恢复" 会把它又塞回列表——所以选择权必须在玩家手里。未勾选的条目原样留在原地，下次扫描仍会列出（不会因为没有恢复就消失，也不会被改动）。

### 3.1 列表里的名字

每条都显示它对应的**记录本名字**，以及证据（记录本 id 与文件路径）：

| 类别 | 名字从哪来 |
|---|---|
| `kuro_<账号>` 文档 | 「库街区 <账号>」；若旧版元数据 `kuromap-accounts.json` 里还留着该账号的 `DisplayName`，显示为「库街区 10383865（旧版叫 Theyun）」——玩家当年自己起的名字，比数字好认 |
| `local` 文档 | 「默认」 |
| 其它 id 的文档 | 该 id 本身 |
| 旧版单文件 | 「旧数据恢复」（即将被创建的记录本名字；旧文件里本来就没有名字这个概念） |

名字的来源要说清楚：**进度文档本身不存名字**（名字只在 `accounts.json` 里），所以被删除的记录本其名字已经随列表项一起消失。能找回名字的唯一地方就是旧版自己的账号元数据 `kuromap-accounts.json` 的 `DisplayName` 字段——这也是上面那张表里唯一"额外"的线索来源。若旧记录本当初根本不在 `kuromap-accounts.json` 里（例如玩家自建的 `acc_xxx`），列表就只显示 id，这是诚实的结果，不编。

统一约定：

- **恢复出来的记录本不绑定库街区账号。** 这些旧点位属于哪个账号只有玩家知道，猜错会把点位写到别人的账号上（正是 `Docs/LocalAccounts_20260926.md` §4.3 反对的那件事）。玩家自己在记录本表里填账号即可。
- **幂等**：每次运行追加到 `<用户目录>\SavedPoints\legacy-recovery.json`（来源路径 + 内容 SHA256 前 16 位 + 结果记录本 id + 动作 + 时间）。再次扫描时同一来源显示「已经恢复过」；来源被改动过（哈希变化）会重新提供。
- 记录本目录无法保存（`catalog.Warning` 非空）时，报告里带上原因，不会假装成功。

## 4. 写出来的文档长什么样，以及为什么敢由托管代码写

`LegacyPointRecovery.BuildDocument()` 逐字段照抄原生 store 自己写出来的形状：

```json
{
  "schemaVersion": 2, "profileId": "<id>", "revision": 1,
  "points": {
    "<stateId>:<pointId>": {
      "sceneName": "World", "nameId": "cx_01", "stateId": 8, "pointId": "…",
      "completed": true, "remoteCompleted": null, "pending": true,
      "localTouched": true, "revision": 1, "acknowledgedRevision": 0
    }
  },
  "syncStates": []
}
```

- `points` 里每条都带 `stateId`/`pointId`（原生 `inspect`/`GetOutbox` 用 `at()` 取它们，缺了就抛）与 `sceneName`/`nameId`（上传接口的 `positionType` 需要）；
- `pending: true` + `localTouched: true` + `remoteCompleted: null` 与 `markerImportLegacyProgress` 的产物一致：绑定账号后这些点位会**被上传**，而不是被判成"云端撤回"而取消；
- 场景名 → stateId 的映射与 `MarkerCompletionStore::SceneState` 必须一致（World 8 / Tethys 900 / Fabricatorium 905 / Avinoleum 903 / Lahai 906 / LowerVault 902 / Darkplain 909 / TimeRiftRuins 910），未知场景一律跳过。

**风险与验证**：这份文档由托管代码直接落盘，绕过了原生 store。如果格式写错，最坏后果是**它在构造时 `Load("local")` 抛异常 → 整个 CoreHost 起不来**（`Docs/KuroSyncCredentialNaming_20260927.md` §3.1 那一类崩溃）。因此回归测试里加了一条**真实 CoreHost 的启动验证**（`Tests/ManagedRuntime/MarkerIpcTests.cs`）：把恢复出来的文档写成 `local`、放进一个只有它的应用数据目录、启动真实 `IMao-CoreHost.exe`，断言 ①宿主能起来 ②点位读回为已完成、按 `stateId:pointId` 与各自场景归位 ③`markerGetOutbox` 把它俩都列为待上传且带 `nameId`/`pointId`。只有这一条通过，才敢说"这份文档原生认"。

## 5. 界面（帮助页最下面）

「旧版本地数据修复」Expander，就在「关于 IMao」之前：

- 三段说明：什么算旧数据（三类 + 具体路径）、为什么新版看不到、恢复不会改动原文件且不会自动绑定账号；
- **「扫描旧数据」** → 一行汇总（找到几处、几处可恢复）+ **一份可勾选的列表**，每行：
  - 左侧复选框（可恢复且未恢复过的默认勾选；"已经恢复过""无法识别"的置灰不可勾）；
  - 第一行（正文）：**记录本名字** · 类别 · 点位数 · 按场景分布 · 状态；
  - 第二行（次要）：`记录本 id：<id>` 与完整文件路径；无法识别时再跟一行原因。
- **「数据恢复」** → 只处理勾选的条目；InfoBar 报告创建/登记了哪些记录本、各多少点、做了什么，并提示去 设置 → 本地点位记录本 里选中它；一个都没勾时提示先扫描再勾选；
- 出错只报不炸：文件不是 JSON、`schemaVersion` 不符、区域名未知、文件过大（32 MB 上限）、IO 失败，都变成一行"无法识别 + 原因"，不会抛给玩家。

## 6. 测试与验证证据

托管（`Tests/ManagedRuntime/LegacyRecoveryTests.cs`，22 条断言）：

- 程序目录自己那份 + 同级旧安装目录那份都能找到；
- 旧版单文件按场景统计、未知场景忽略、空 `id` 忽略；
- 未登记文档被**原地登记**（字节不变、绑定从 id 推出）；不可识别文档只报告不登记；
- `deleted\` 文档复制回自己的 id、原副本保留；
- 旧版单文件变成**一本**新记录本、**未绑定**、点位数为并集去重后的数量；
- 写出的文档满足原生 `Load` 的四条校验，且每条点位带齐原生读取器会碰的字段、`completed`/`pending`/`localTouched`/`remoteCompleted: null` 全部就位；
- 来源文件字节不变；**再次恢复不重复创建**，再次扫描全部标记为已恢复；
- 每条都报出记录本名字（`旧数据恢复` / `默认` / `库街区 777`），`kuro_<账号>` 还会带上 `kuromap-accounts.json` 里的旧名字；
- **只恢复勾选的条目**：未勾选的那条既不被登记、字节也不变，且下次扫描仍可恢复；
- 区域名全未知的文件只给一行原因，不会造出一本空记录本；
- 日志文件是 `version 1` 的 JSON，条目数和来源路径都在。

原生（真实 CoreHost，`MarkerIpcTests`）：见 §4 的三条断言。

运行方式：

```
dotnet run --project Tests\ManagedRuntime\ManagedRuntime.csproj -c Release                        # 815 条断言（含恢复 22 条）
dotnet run --project Tests\ManagedRuntime\ManagedRuntime.csproj -c Release -- out\map-test        # 追加真实 CoreHost 的 IPC 用例
```

结果：**全部通过**（`All managed runtime tests passed.`）。

## 7. 本轮没做的

- **格式损坏的 `profiles\local.json` 不会自动隔离**：扫描会把它标成"无法识别"，但不会重命名它；如果被破坏的正是 `local`，程序仍然起不来（`Docs/KuroSyncCredentialNaming_20260927.md` §4.5 的 C++ 加固）。本轮不动 C++。
- **没有文件夹选择器**：只自动扫父目录一层。旧程序目录被搬到别处（U 盘、另一个盘符）的玩家仍需手动把 `SavedPoints` 拷回来再点扫描。文件夹选择器需要 `FileOpenPicker` + `InitializeWithWindow`，留作下一步。
- **不恢复路线**：手绘路线本来就有自己的迁移路径（`LoadEditRouteData` 从程序目录 `SavedRoutes\*.json` 复制），自动路线在 `SavedRoutes\Auto\<id>\`，`deleted\` 里那份会被复制回来（`TryDelete` 搬的就是它），但本轮不去动它。
