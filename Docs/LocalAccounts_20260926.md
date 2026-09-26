# 本地多账本与库街区账号绑定（设计草案）

- 日期：2026-09-26
- 分支：`feat/local-accounts`（起点 `main` = `28e6065`）
- 状态：**已实现（阶段 A–E，见文末「实现状态」）**；本文档保留为设计与判据记录。
- 相关历史文档：`Docs/KuroProgressSyncPlan_20260916.md`（已归档于 `Docs/archive/`）、`Docs/archive/MarkerFeatures_20260908.md`

---

## 0. 一句话

把"档案"变成玩家看得见、能命名、能增删的**本地账本**；库街区账号是贴在账本封面上的**绑定关系**，不再是账本的名字。
**同步只作用于当前选中的账本；任何时候都不会因为同步而切换账本。**

---

## 1. 背景：三个真实症状与证据

三个症状都表现为"旧版本收集过的点，新版地图上显示为未收集"。

### 1.1 症状 ①：翻错账本（读到了另一本）

现状：账本 id 同时也是文件名，玩家能看到哪一本，完全由启动时的选择决定：

| 位置 | 代码 |
|---|---|
| 原生 store 启动固定加载 `local` | `MarkerCompletionStore.h` 构造函数（当时约 17 行） |
| 只有 `markerSelectProfile` 能换账本 | 同文件 `Execute()`（当时约 38~42 行） |
| 启动时按 `kuromap-accounts.json` 的 `ActiveProfile` 选择 | `CoreHostService`（构造与 `EnsureStartedLockedAsync`，当时约 52、150 行） |
| **同步服务也会换账本** | `KuroProgressSyncService.cs` 当时第 58、99 行（预览与应用各一次） |

也就是说：**"预览同步"是一个只读动作，却会把地图切到另一个账本。** 如果玩家原来的点在 `local`，而同步填的是 `kuro_<uid>`，地图立刻全变未收集——数据一个字节都没少，只是不再被读取。

探针实测（`out/analysis/marker-legacy-probe.cpp`，驱动真实 `MarkerCompletionStore`）：

```
[A] local 账本读旧文件            -> Completed = 1
[B] 同一份数据换成 kuro_... 账本  -> points = 0，Completed = 0（文件仍在，只是没被导入）
```

### 1.2 症状 ②：同步把本地独有的勾擦掉

`markerApplyRemote` 在"该区域已经初始化过"时**不看 `mode`**，一律以云端为准（`MarkerCompletionStore.h` 当时约 275~285 行）：

```cpp
} else if (point.value("pending", false)) {
    // 保留本地待上传的修改
} else if (point.value("completed", false) != remote || ...) {
    point["completed"] = remote;   // 本地有、云端没有 -> 直接改为未完成
}
```

问题在于"本地有、云端没有"有两种完全不同的含义，代码把它们当成了一种：

- 云端曾经有过、现在撤回了 → 应该跟着取消；
- 云端从来没有过（本地独有）→ **不该取消**，应该排队上传。

而计划文档早就写清楚了正确规则（`Docs/archive/KuroProgressSyncPlan_20260916.md`「后续合并规则」的 B/L/R 表）：

| 条件 | 处理 |
| --- | --- |
| L = R | 已一致，更新基线 |
| L ≠ B 且 R = B | 本地修改，上传明确目标状态 |
| L = B 且 R ≠ B | 云端修改，原子应用到本地 |
| 无可信 B 且 L ≠ R | 首次合并，不猜测新旧 |

`remoteCompleted` 正是这里需要的基线 B。**实现的非初始化分支没有使用它。**

探针实测：

```
[C] 首次同步(mode=merge)     -> completed=1 pending=0 remoteCompleted=true
    下一次应用(mode=merge)   -> completed=0              ← 被取消
    outbox entries = 0                                   ← 不进待上传队列，静默永久丢失
[D] 仍然 pending 的本地记录  -> 被保护，completed 保持 1  ← 所以受害的只有"非 pending"的点
```

配套的两处放大效应：

- **界面承诺与实现相反**：设置页原文"同步只做并集…不会取消任何点位"，`KuroProgressSyncService` 的类注释也写着 "Merging only ever adds"。
- **预览看不出来**：`KuroSyncPlan.cs` 把这类点算成 `ToUpload`（"待推送"）并让 `NeedsApply = true`，而原生算出来的 `willRemove` **没有进入 UI 模型**（预览表只有 本地点位数 / 待推送 / 云端点位数 / 待拉取）。玩家看到"待推送 495"，点下去的实际效果是"删掉 495、一个都不上传"。

### 1.3 症状 ③：最早那本记录只对 `local` 生效

`MarkerCompletionStore.h` 当时约 413~414 行：

```cpp
const auto legacy = root / "account_1.json";
if (profile == "local" && std::filesystem::exists(legacy)) { ...导入... }
```

`account_1.json` 是重写前版本写在程序目录里的记录（`GetCurrentPath()\SavedPoints\account_1.json`，见历史提交 `1a8751c`）。**只要玩家的活动账本是 `kuro_<uid>`（当年登录过库街区的基本都是），这个文件永远不会被导入。** 这是历史遗留，不是 9.18.2 引入的，但症状与 ①② 完全一样。

本机实测（只读检查，未改动）：`kuromap-accounts.json` 写着 `ActiveProfile=kuro_10383865`，因此程序读 `profiles\kuro_10383865.json`（153 点 / 150 已完成，5 个区域 `initialized=true`），而旁边的 `account_1.json` 里那条 `cx_03` 点位**不会**通过迁移路径进来。

### 1.4 顺带确认：读取路径在两个版本之间没有改过

`git diff v2026.9.9.6 v2026.9.18.2` 中与本地完成记录相关的改动只有：`MarkerCompletionStore.h` 新增 `markerPreviewSync`、`Completed`/`CompletedIds` 增加远端兜底，以及 `DrawItemBase.cpp` 的"空 points 视为未提供"。**旧档案导入、`markerApplyRemote`、`markerSetCompletion`、档案选择、点位资源、点位/分类 id 全部逐字节未变。**

所以"升级后旧点变未收集"不是"新版本不认识旧格式"，而是本节 ①（换了账本）与 ②（改了内容）两件事，两者都只在**同步真的跑过一次**之后才会发生。

---

## 2. 目标与非目标

### 目标

1. 新用户只有一本账本，默认未绑定库街区；不登录、不联网也能正常用全部本地点位功能。
2. 绑定由玩家显式完成；**同步只作用于当前账本，并且永远不会切换账本**。
3. 老玩家的每一份历史数据都有明确去处：`profiles\local.json`、`profiles\kuro_*.json`、`SavedPoints\account_1.json` 全部可见、可导入、可回退。
4. 一个库街区账号最多绑定一本账本（建议，见 §12）。
5. 同步语义回到计划文档的 B/L/R 表：**不会因为"云端没有"而丢掉本地完成，也不会因为"本地没有"而忽视云端撤回。**

### 非目标

- 不做多设备并发写入的强一致（仍按"单一写入者"设计）。
- 不做云端多账号同时同步、不做服务器端版本号/CAS。
- 不自动合并、不自动删除、不自动改写任何历史文件。
- 不动覆盖层、窗口、DPI、定位算法。

---

## 3. 术语

| 术语 | 含义 | 现有对应物 |
|---|---|---|
| 账本（ledger） | 一份本地点位完成记录 | 现在的"档案 / profile" |
| 账本目录 | 账本列表与绑定关系 | **新增** `SavedPoints\accounts.json` |
| 绑定 | 账本 → 库街区账号 | 现在只隐含在账本名字里 |
| 凭据 | 该账本在本机的库街区 token | `KuroSync\credentials\<ledgerId>.json` |
| 当前账本 | 地图与所有命令作用的那一本 | 原生 store 的活动文档 |

**关键区分**：绑定是元数据，凭据是敏感数据，进度是用户数据。三者分开存放、分开失效，任何一项出问题都不影响另外两项。

---

## 4. 数据模型

### 4.1 账本目录 `SavedPoints/accounts.json`（新增）

```json
{
  "version": 1,
  "activeAccountId": "default",
  "accounts": [
    { "id": "default", "name": "默认", "kuroAccountId": "", "createdAtUtc": "2026-09-26T09:00:00Z", "lastUsedAtUtc": "2026-09-26T09:20:00Z" },
    { "id": "kuro_10383865", "name": "主号", "kuroAccountId": "10383865", "createdAtUtc": "2026-09-08T04:12:00Z" }
  ]
}
```

约束：

| 字段 | 规则 | 理由 |
|---|---|---|
| `id` | `[A-Za-z0-9_-]{1,96}`，**不可修改** | 直接当文件名用；与现有原生 `ValidateProfile`、`KuroTokenVault.ValidateProfile` 规则一致 |
| `name` | 去空白后 1~40 字符，允许中文/emoji，允许重名（UI 提示） | 玩家自己的叫法；不进文件名，所以不限制字符集 |
| `kuroAccountId` | 空（未绑定）或 1~24 位数字；**全局唯一** | 与现有 `LocalMarkerProfileSelection` 认可的长度范围一致 |
| 数量 | ≤ 32 本 | 有界，避免异常目录拖慢启动 |
| 文件 | ≤ 64 KiB，原子替换写入 | 与项目其它用户数据一致 |

迁移/兼容规则：未知字段在写回时丢弃，靠 `version` 升级；文件损坏、超限或全部条目非法 → **回退到默认账本并向玩家报明确原因，且不覆盖原文件**。

### 4.2 账本进度 `SavedPoints/profiles/<ledgerId>.json`（不变）

沿用现有 `schemaVersion: 2` 文档，不加新字段。`syncStates`（每个区域的云端基线、`remoteIds`、`initialized`、`initialMode`）**随账本走**——这正是"这本账本第一次同步"能被精确定义的原因，也是本设计不引入跨账本串数据的基础。

### 4.3 凭据 `KuroSync/credentials/<ledgerId>.json`（扩展）

现记录只有 `{ ciphertext, savedAt }`（DPAPI 保护，见 `KuroTokenVault.cs`）。**新增 `accountId`**：

- 新写入的凭据必须带 `accountId`；
- 旧文件没有该字段时：若账本 id 形如 `kuro_<数字>`，按名字推断；否则标记为"账号未知"，要求玩家重新连接一次后才能开启同步；
- 同步前置校验：凭据的 `accountId` 必须等于绑定值，否则**拒绝同步并提示"这本账本的凭据属于另一个账号，请重新连接"**，绝不猜。

### 4.4 路线 `SavedRoutes/Auto/<ledgerId>/…`（不变）

路线本来就按档案分目录（`RoutePlanStore` 的 `Folder(profile)` / `Path(profile, id)`）。这意味着**改账本名字不能改 id**，否则路线和进度会脱钩。

### 4.5 旧文件（只读，永不修改）

| 文件 | 用途 | 规则 |
|---|---|---|
| `SavedPoints/account_1.json` | 重写前版本的完成记录（写在程序目录里，启动时被复制到用户目录） | 只作为一次性导入来源 |
| `kuromap-accounts.json` | 旧账号元数据 | 只用于**预选**当前账本，之后不再读写 |

---

## 5. 从现状迁移：账本整理

### 5.1 触发条件

首次运行本版本，且满足任一条：`accounts.json` 不存在、`profiles\` 下有 ≥2 个文件、存在 `account_1.json`、存在 `kuromap-accounts.json`。

### 5.2 扫描产物（只读）

对每个 `profiles\<id>.json` 报告：点位数 / 已完成数 / 最近修改时间 / `syncStates` 是否已初始化 / 是否能从旧元数据推断绑定。
另外单列 `account_1.json` 为"旧格式记录（N 个点，未导入）"。

### 5.3 交互（一屏搞定，可跳过）

```
发现 2 本本地账本 + 1 份旧格式记录：
  [x] 本地         153 个点，最后使用 09-25  绑定：[未绑定 ▾]
  [ ] 库街区 10383865  1 个点，最后使用 09-08  绑定：[10383865 ▾]
  ( ) 旧格式记录 account_1.json：1 个点  →  [导入到：库街区 10383865 ▾]
当前使用：[库街区 10383865 ▾]
```

- 默认勾选"最后使用时间最新"的那一本；`kuromap-accounts.json` 的 `ActiveProfile` 只用于预选。
- 名字可直接改；绑定可直接填/清空；旧记录可"导入到某本"或"暂不导入"。
- **只读扫描 + 原子写 `accounts.json`；不合并、不重命名、不删除任何进度文件；选错可以再改。**

### 5.4 导入旧格式记录

导入到目标账本时，每条记录写成：`completed = true`、`localTouched = true`、**`pending = true`**、`revision` 推进。

`pending = true` 是硬要求：否则这些点会在第一次同步时被 §1.2 的旧逻辑当成"云端没有"而擦掉（探针 [C]）。这也是症状 ③ 的真正解法。

---

## 6. 同步语义（与本设计同时生效）

### 6.1 硬前置条件（任一不满足就拒绝，且不写任何文件）

1. 当前账本 == 被同步账本（同步**不允许**调用 `markerSelectProfile`）；
2. 绑定非空（`kuroAccountId` 有值）；
3. 本机存在该账本的凭据，且凭据 `accountId` 与绑定一致；
4. 云端读取非空且完整（沿用现有的"读取为空则报错"，另外对"分页/中途失败"继续按失败处理）。

### 6.2 去掉隐式切换账本

删除 `KuroProgressSyncService` 中的两处 `markerSelectProfile`（当时第 58、99 行）。同步界面只显示"当前账本：X（已绑定 Y）"，并提供显式的"切换账本"入口。

### 6.3 `markerApplyRemote` 改为按 B/L/R 表执行

以 `remoteCompleted` 为基线 B，`completed` 为本地 L，`remote` 为云端 R：

| 情况 | 含义 | 处理 |
|---|---|---|
| R = true | 云端有 | `completed = true`、`remoteCompleted = true`、`pending = false` |
| R = false 且 B = true | 云端撤回 | `completed = false`、`remoteCompleted = false`、`pending = false` |
| R = false 且 B ≠ true 且 L = true | **本地独有** | **保持 `completed = true`，`pending = true`（排队上传）** |
| R = false 且 L = false | 两边都无 | 不动 |

`pending = true` 的记录继续沿用现有保护（预览阶段跳过、上传成功后按 revision 确认）。

### 6.4 预览必须说真话

- 原生已经算好了 `willRemove`（`markerPreviewSync` 返回值里就有），**把它接进 UI 模型**（`KuroSyncPlan.cs` / 设置页表格），显示为"将被取消 N"；
- "待推送"必须等于**真正会进 outbox 的数量**，不能拿"本地独有"当"待推送"；
- 应用前后的计数不一致时，同步结束要显示实际写入数而不是计划数。

### 6.5 可以先单独发布的最小修复

如果不想等整套账本模型，可以先只做两件事，风险与改动都最小：

1. `markerApplyRemote` 的非初始化分支加上 B/L/R 判定（§6.3 的第三个分支）；
2. 预览表加上"将被取消 N"。

这两条能立刻止住"同步后旧点变未收集"，且不涉及迁移与 UI 重构。**建议：即使整套模型延后，这两条也应该先发。**

---

## 7. 界面

设置页新增"本地点位账本"区域：

| 元素 | 行为 |
|---|---|
| 账本列表 | 名称 / 点位数 / 最近使用 / 绑定状态；点击切换当前账本（显式动作） |
| 新建账本 | 输入名称；id 自动生成（`acc_` + 随机）；默认不绑定 |
| 改名 | 只改 `name`，不动 id |
| 绑定/解绑库街区 | 走现有浏览器扩展连接流程；绑定成功后提示"这本账本以后与账号 X 同步" |
| 导入旧格式记录 | 仅当 `account_1.json` 存在时出现 |
| 删除账本 | 二次确认 + 提示先备份；进度文件移入 `profiles\deleted\`（保留，不真删） |
| 打开账本目录 | 复用现有"打开点位目录" |

同步区域只显示当前账本与状态（已绑定 / 未绑定 / 凭据失效 / 最近结果），**不再有"同步档案 ID"输入框**。

另外新增"账本体检"导出（一个 JSON，列出每本账本的点位数、最近修改、绑定、凭据状态）。以后遇到这类玩家反馈，直接让他发这个文件，不必再靠猜。

---

## 8. 兼容与回退

| 场景 | 要求 |
|---|---|
| 玩家从新版本回退到旧版本 | 旧版本只认 `kuromap-accounts.json` 的 `ActiveProfile` 与 `profiles\<id>.json`。因此：**①账本 id 绝不改名；②我们继续把当前账本同步写回 `kuromap-accounts.json` 的 `ActiveProfile`（只写这一个字段）**，让回退后的旧版本仍能看到同一本账本 |
| 旧版本 → 新版本 | 见 §5，只读扫描 + 玩家确认 |
| 未绑定 / 凭据失效 / 绑定与凭据不符 | 同步按钮置灰或直接拒绝，并给出明确文案；**不允许"跳过校验照常同步"** |
| 账本目录损坏 | 回退到默认账本 + 明确报错；不覆盖原文件 |

---

## 9. 测试清单

原生（`IMaoMarkerTests`）：

- 两本账本互不串：点位、`syncStates`、outbox、`Completed`/`CompletedIds`；
- B/L/R 四种组合各自的处理（含"云端撤回"与"本地独有"两条相反结果）；
- 本地独有点在两次同步后仍然存在且进入 outbox；
- 旧格式导入后 `pending = true`，且不被第一次同步取消；
- 非法账本 id / 目录损坏 / 超限时的回退行为。

托管（`Tests/ManagedRuntime`）：

- 同步不调用 `markerSelectProfile`（断言"选择账本在预览与应用前后不变"）；
- 绑定与凭据不一致时拒绝同步，且所有用户文件字节不变（哈希断言）；
- 账本目录的读写：重名、超限、损坏、原子写；
- 迁移对话框只读：扫描前后所有历史文件哈希一致；
- 自动同步只作用于当前账本；当前账本未绑定时暂停并说明原因。

真实 CoreHost（`IMao-WinUI` 集成）：

- 启动读 `accounts.json` 选择账本；没有该文件时行为与现在一致（默认账本 + 旧格式导入一次）；
- 切换账本后路线、候选、攻略记录都跟着换，且旧账本数据不变。

---

## 10. 实施阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| 1 | 账本目录 + 启动选择（不含同步改动） | 现有玩家升级后地图显示与升级前完全一致（同样的点位数）；回退到旧版本仍能看到同一本账本 |
| 2 | 账本整理对话框 + 旧格式导入 | 扫描全程只读（哈希断言）；导入后旧点数为 +N 且为待上传状态 |
| 3 | 绑定 + 凭据校验 + 界面 | 未绑定/不匹配时同步被拒绝且有明确文案；绑定后能正常双向同步 |
| 4 | 同步语义修正（§6.3/6.4） | 探针 [C] 场景变成"保持完成 + 进入待上传"；云端撤回的场景仍能正确取消 |
| 5 | 账本体检导出 + 文案整理 | 导出的 JSON 能独立回答"这个玩家有几本账本、哪本有数据、绑定了谁" |

阶段 1~3 是本设计的核心；阶段 4 可以提前独立发布（§6.5）。

---

## 11. 风险

1. **回退兼容依赖 id 稳定**（§8）。任何"顺手改个名字更整齐"的想法都会破坏它。
2. **迁移对话框是唯一会让玩家搬数据的地方**，必须可回退、必须只读扫描、必须保留原文件。
3. **多账本 + 自动同步**：自动同步必须绑定到当前账本；如果玩家切了账本，计时器要重置，不能用上一个账本的比较结果写新账本（现有 `gate` 锁可以复用）。
4. **批量上传未实测**：计划文档已标注"批量上传（只实测单点往返）"未验证。导入旧记录可能一次产生几百个 `pending`，需要在实现前先用测试账号验证限流与 429 退避。
5. **同账号绑多本账本**：会造成"两边都不对"的观感（A 推上去、B 拉回来）。建议禁止，见 §12。

---

## 12. 待确认（需要拍板后才能动代码）

| # | 问题 | 建议 |
|---|---|---|
| 1 | 一个库街区账号能否绑定多本账本？ | **禁止**（创建/绑定时校验），UI 给出原因 |
| 2 | 旧记录 `account_1.json` 导入到哪里？ | 让玩家选，默认当前账本 |
| 3 | 导入后是否自动标记为待上传？ | **是**（否则又被同步擦掉） |
| 4 | 默认账本的 id 用什么？ | 沿用 `local`（与历史文件、路线目录完全兼容） |
| 5 | 是否保留 `local` / `kuro_<数字>` 的历史 id 逐字不变？ | **是**（回退兼容的硬要求） |
| 6 | 是否要"账本体检"导出？ | 要，排查这类反馈的固定入口 |
| 7 | 删除账本是移到 `deleted\` 还是真删？ | 移走保留（本项目一贯的"想删就删但不丢数据"风格） |

---

## 附录 A：本文引用的证据

| 证据 | 位置 |
|---|---|
| 三个症状的探针实测 | `out/analysis/marker-legacy-probe.cpp`（运行输出见 §1.1 / §1.2） |
| 读取路径两版未变 | `git diff v2026.9.9.6 v2026.9.18.2 -- IMao-Core/src IMao-WinUI` |
| 同步命令与账本切换 | `IMao-WinUI/Services/KuroProgressSyncService.cs`、`IMao-Core/src/Runtime/MarkerCompletionStore.h` |
| 计划文档的 B/L/R 表 | `Docs/archive/KuroProgressSyncPlan_20260916.md`「后续合并规则」 |
| 旧档案可能带 `syncStates` 的警告 | `Docs/archive/MarkerFeatures_20260908.md` §6 |
| 旧格式文件的来源 | 历史提交 `1a8751c`（`GetCurrentPath()\SavedPoints\account_1.json`） |

## 附录 B：不在本设计范围内、但相关的问题

1. **已经被 ② 擦掉的记录**：本设计能防止将来再发生，但擦掉的勾需要另外的手段找回（`account_1.json` 与各 `profiles\*.json` 都还在盘上，"从旧文件重新导入 + 重新上传"是可行路径，需要单独设计一次"进度修复"流程）。
2. **`markerResolveConflict` / `markerSetSyncState` 没有生产调用者**：冲突在当前流程里永远不会被玩家决策，预览里也不显示。属于后续清理项。
3. **`markerCopyLocalProgress` 同样没有调用者**：本设计的"旧记录导入"会替换它的用途，实现时应该明确取舍（接上界面或删除）。

---

## 实现状态（2026-09-26 完成，分支 `feat/local-accounts`）

| 提交 | 阶段 | 内容 |
|---|---|---|
| `04bfd3f` | A（§6.2/6.3/6.4） | `DesiredCompletion()` 成为预览与应用的唯一判据；预览新增 `willQueue`；托管精确计数 + "将被取消"列；同步不再调用 `markerSelectProfile` |
| `a3bd2b5` | B（§4.1/5/8） | `IMao-WinUI/Services/LocalAccountCatalog.cs` + 启动按账本目录选当前账本 |
| `407a34c` | C（§5.4） | 原生 `markerImportLegacyProgress` + `Describe()` 计数 |
| `9e9618d` | D（§5/6.2/7） | 设置页"本地点位账本"区（列表/切换/新建/改名/绑定/删除/导入/体检）+ 同步目标改成当前账本 |
| `b3eefbf` | E（§4.3/7/8） | 凭据记录 `accountId` 并校验；旧元数据写回 `ActiveProfile`；账本体检导出 |

### 与本文档的差异（实现时的决定，评审时请看这里）

1. **"账本整理"不是启动弹窗，而是"自动无损播种 + 设置页内的账本区"**（§5.3 原写一屏对话框）。
   理由：启动弹窗需要动 WinUI 启动流程且难以自动化验证；播种本身不合并、不改名、不删除，与弹窗等价，
   而改名/绑定/导入/删除都在设置页里可反复操作。**功能上没有缺口，交互形式与文档不同。**
2. **同步目标不再是手填的"同步档案 ID"**：它是当前账本，凭据同样按账本 id 存放；`kuroSyncProfileId`
   这个设置项已删除（旧值不再使用）。历史玩家的账本 id 恰好就是 `kuro_<账号>`，因此旧凭据文件仍然对得上。
3. **`accountId` 只有在新版扩展连接时才会写入**；旧扩展不发送它时按"未知"处理，不视为冲突（§4.3 的兼容规则）。
4. **删除账本会把进度、凭据、路线一起移到 `SavedPoints\deleted\<时间>-<id>\`**，不是只移进度文件。
5. **默认账本的显示名是"默认"**（id 仍是 `local`）。
6. **界面用词是「本地点位记录本」**（用户 2026-09-26 评审时定的）：玩家看到的所有文案（设置页、同步区、
   警告与报错）都叫"记录本"，不再叫"账本"；代码里的类型、文件与字段仍叫 ledger / `LocalAccountCatalog` /
   `accounts.json`，**只有文案改名，标识符不动**（改名会牵动文件名与测试，收益为零）。
7. **记录本区是一张表，不是一叠文本 + 一排按钮**（同一轮评审定的，最终形态）：
   表头 `选择 / 名称 / Kuro ID / 已完成`（Kuro ID 就是绑定的库街区账号，未绑定时显示 `—`；
   **已完成只显示本地点位数，不带分母**——分母容易被读成"地图总点位"或"云端点位数"，评审时已删掉）；
   **首列是单选按钮，勾选哪一行就把哪一本设为当前记录本**（这是唯一的切换入口，同步仍然不许切换）；
   下面只有 **新增 / 修改 / 删除** 三个主按钮，加 `导入旧记录 / 导出体检 / 打开目录` 三个次要按钮。
   新增与修改共用同一个对话框（名称 + 绑定账号），修改时先改绑定再改名，改名失败会把绑定回滚。

### 尚未做（有意留下）

- **未实机验收**：16:10/4K 那类真机检查这里同样适用——本功能需要在真实账号上跑一次"切换账本 → 预览 → 应用 → 断网 → 重启"的完整路径，本分支只做了仓库内测试。
- **§8 的"批量上传未实测"仍然成立**：旧记录导入可能一次产生几百个 `pending`，写接口的限流仍需实测账号验证。
- **死命令未清理**：`markerCopyLocalProgress`、`markerResolveConflict`、`markerSetSyncState` 仍无生产调用者（附录 B）。
- **§9 里没写测试的部分**：两本账本"路线/攻略窗口互不串"只有既有回归覆盖，没有针对新账本列表的新增用例；
  "同步过程中玩家切换账本"只有 `EnsureActiveProfileAsync` 的前置校验，没有并发用例。

### 验证证据

| 项 | 结果 |
|---|---|
| 原生 `IMaoMarkerTests`（含 §6.3 基线回归与 §5.4 导入） | 通过 |
| 原生 `IMaoOptimizationTests` | 通过 |
| 托管 `Tests/ManagedRuntime`（含账本目录 19 条、凭据 6 条、真实 CoreHost 导入 4 条） | 全部通过 |
| 全量 `scripts/Test-Runtime.ps1` | 通过（证据 `out\phase-final-runtime`） |
| `IMao-WinUI.exe`（XAML + code-behind）构建 | 通过，无新增警告。⚠️ **要双击启动必须用自包含构建**（`-r win-x64 --self-contained true`）；不带 RID 的 `dotnet build -p:Platform=x64` 产出的是框架依赖版，会被 `.NET Runtime` 拒绝启动 |
| 实机首次启动（`out\map-test`，2026-09-26 17:13） | 通过：`accounts.json` 按设计播种（`kuro_10383865` 为当前账本 + `local`），`kuromap-accounts.json` 只被写回 `ActiveProfile` 且其余字段保留，app 与 CoreHost 均正常运行 |
| 记录本表格版实机启动（`out\map-test`，2026-09-26 17:37） | 通过：自包含重建 + 同步交付树后，app（187 MB）与 CoreHost（1.5 GB）正常运行，`startup.log` 记 `root=out\map-test`，无错误 |
| `tools/KuroSyncBridge` 构建 | 通过 |
| 探针 `out/analysis/marker-legacy-probe.cpp` | [A]~[E] 五景复核通过 |
