# IMAO 在 Steam Deck / Proton 上的验证与使用指南

> 面向：手上有 Steam Deck 的玩家与维护者。
> 本文所有关于本程序的陈述都标注**证据强度**，请勿混用：
>
> | 标记 | 含义 |
> |---|---|
> | **【代码事实】** | 在某文件某行能读到的确定行为。可以直接照做。 |
> | **【合理推断】** | 由代码事实 + Wine/Proton 的已知结构推出，但**未在本机验证**。可能是错的。 |
> | **【待实机】** | 只有真机跑过才能知道，代码给不出答案。 |
>
> 同样区分三种**验证状态**，不得互相替代：**自动化测试通过**（仓库内模拟，本机可复现）、
> **Proton 环境验证通过**（Linux + Proton 上真的跑起来过）、**Steam Deck 实机验证通过**（掌机上真的跑起来过）。
> 截至本文写作时，只有第一项成立。

---

## 1. 本次修复了什么：路径安全边界

### 1.1 现象与根因

玩家反馈：`资源目录不能包含符号链接或目录联接：Z:\home`。

**【代码事实】** `RejectLink()`（`IMao-WinUI.Core/Updates/UpdateStorage.cs`）原来从给定路径**逐级检查到卷根**。
**【合理推断】** Wine 只对"路径中那一段本身是符号链接"的组件报 `FILE_ATTRIBUTE_REPARSE_POINT`，而 SteamOS /
Steam 客户端上这类组件是平台自己造的、且都在本程序目录的**上方**（`~/.steam/steam`、不可变根发行版的
`/home` → `/var/home`、Wine 的 `Z:` 盘映射）。于是"本程序自己放的每一级都是真实目录"的安装仍被判死。
报错里的 `Z:\home` 正是这样一个**位于安装目录之上**的组件。**具体是哪一个组件导致这台机器报错，需实机确认。**

### 1.2 修法：受管根目录（owned root）边界

**【代码事实】** `RejectLink(path, ownedRoot)`：

- 每个调用点声明自己拥有的目录——程序更新用**安装根**，资源更新用**更新根**（其边界再上抬一级到
  `%LOCALAPPDATA%\IMao-WinUI`，见 §1.5），程序自带资源用 **`Assets` 根**；
- 检查从目标向上走到该边界为止，**边界本身也在检查范围内**；
- 边界**上方**的链接只写一条诊断，不参与判定；
- 未声明边界的调用点（`tools/UpdatePublisher`）保持原来的"查到卷根"行为；
- **没有任何操作系统判断，没有任何路径白名单**。

### 1.3 为什么这不削弱安全

- 真正危险的是**受管目录内部**的链接——解压、版本切换、回滚、递归删除都在那里，边界内一律照旧拒绝。
- 边界上方的链接无法把边界内的路径重定向出去：那些组件对所有路径都是**共同前缀**，解析前后包含关系不变
  （`SafeChild` 依赖的正是这一点）。
- 玩家把安装目录本身放在链接下，攻击者拿不到新能力：程序就是从那条路径启动的。

### 1.4 诊断日志

**【代码事实】** `IMao-WinUI.Core/Updates/UpdatePathDiagnostics.cs`，三类，每类最多 64 行、按内容去重、不含凭据，
经 `IMao-WinUI/App.xaml.cs:58` 落到 `%LOCALAPPDATA%\IMao-WinUI\Logs\startup.log`（前缀 `compat `）：

| 类别 | 含义 | 下一步 |
|---|---|---|
| `path-mapping` | 受管目录**上方**有链接。**不影响功能** | 无需处理，仅用于确认环境形状 |
| `link-refusal` | 受管目录**内部**有链接，已拒绝（带边界路径与解析目标） | 删除该链接，或把安装/更新目录移到没有链接的位置 |
| `path-inaccessible` | 路径存在但读不到属性 | 权限/盘符问题，不是安全判定 |

### 1.5 安全边界复审（本轮，按审查项逐条）

| 审查项 | 结论 | 依据 |
|---|---|---|
| **可变更的上级目录链接** | 原来"边界上方的一切都不看"确实放开了**边界正上方那一级**。已针对本程序**确实拥有**的目录收窄：资源更新的边界由 `...\ResourceUpdates` 上抬到 `%LOCALAPPDATA%\IMao-WinUI`（这个目录是本程序自己建的），因此在那里放链接**仍然被拒绝**。安装根**无法**再上抬——它的父目录（`C:\Dapps`、`steamapps\common`）是玩家选的，不是本程序的，**不能声称拥有**。 | `ResourceSnapshotService.UpdateRootBoundary`、`ResourceUpdateBootstrap.cs:66` |
| **路径别名** | 8.3 短名、`\\?\` 前缀、尾随点/空格都不会造成越界：`SafeChild` 的包含判断是**字符串前缀**，别名解析后仍落在同一前缀内；`RejectLink` 逐级检查时别名的重解析属性同样可见。盘符映射（`Z:`）是唯一"同名不同地"的别名，它正是本次要放行的平台布局。 | `UpdateStorage.SafeChild`、`RejectLink` |
| **检查→操作的目录变化（TOCTOU）** | 这是**既有**窗口，不是本次引入：原实现同样先检查后操作。本次把遍历**缩短**了，窗口只会更小。要真正关闭它必须改成基于句柄的 I/O（`FILE_FLAG_OPEN_REPARSE_POINT` + 句柄级路径校验），**在 Wine 上的可靠性低于 Windows**，属于"用更大的 Proton 风险换一个本威胁模型之外的攻击者"。**不做**，在此记录为已知边界。 | `UpdateStorage.RejectLink` 注释 |
| **删除越界** | 逐个复核全部 `Directory.Delete`/`File.Delete`：`RemoveScratch`、`PruneUnreferencedVersions`、`SweepStaleStaging`、`SweepStaleDescriptors`、`RemoveAsync`、`InstallReleaseAsync` 的 finally——**每一处都先 `RejectLink` 再删**，且中途枚举到的每个条目也单独检查。 | 见 §1.6 的核对清单 |
| **解压越界** | 程序包与资源包两条解压路径都在**创建目录之前**检查目标（改前是"先建目录再检查"，会在链接另一侧留下空目录）。 | `ProgramPackageValidation.cs:227`、`UpdateService.ExtractPackageAsync` |
| **回滚越界** | 回滚走 `ValidateInstalledAsync` / `ReadAndValidateAsync`，两者都以各自的受管根为边界。 | `ProgramUpdateStore.cs:164`、`ResourceSnapshotService.cs:677` |

**本轮实际改了两处**（都是"发现问题才改"，不是预防性重构）：

1. **`ExtractEntriesAsync` 的边界曾默认取 `destination`**，这让**没有声明边界**的发布器调用点悄悄变松了。
   已改为：没传边界就是**原来的无边界检查**，Mirror酱路径改为显式传入自己的暂存目录。
   新增用例 `a caller that names no boundary keeps the unbounded check it always had` 钉住。
2. **资源更新边界上抬到 `%LOCALAPPDATA%\IMao-WinUI`**，并把该检查移到"取锁"之前（取锁会创建更新根）。
   新增用例 `a link at the highest directory this program owns is refused, not relocated` 与
   `a boundary that does not contain the update root is refused at construction`。

### 1.6 破坏性操作 × 边界 核对清单

| 操作 | 位置 | 边界 |
|---|---|---|
| 暂存清理（递归删） | `ProgramUpdateStore.RemoveScratch` | `Root`（`InstallRoot\ProgramUpdates`） |
| 旧版本目录裁剪（递归删） | `ProgramUpdateStore.PruneUnreferencedVersions` | `Root` |
| 资源暂存清扫（递归删） | `ResourceSnapshotService.SweepStaleStaging` | `UpdateRootBoundary` |
| 描述符缓存清扫（删文件） | `ResourceSnapshotService.SweepStaleDescriptors` | `UpdateRootBoundary` |
| 区域包删除（递归删） | `UpdateService.RemoveAsync` | `OwnedRootFor(directory)` |
| 安装暂存清理（递归删） | `UpdateService.InstallReleaseAsync` finally | `UpdateRootBoundary` |
| 解压写入 | `ProgramPackageValidation.ExtractEntriesAsync` / `UpdateService.ExtractPackageAsync` | `Root` / `work` |
| 版本切换（目录移动） | `ProgramUpdateStore.PrepareAsync`、`UpdateService.InstallReleaseAsync` | `Root` / `OwnedRootFor(target)` |

---

## 2. 启动协议——**先读这一节，否则下面的步骤全错**

### 2.1 正常启动链 **【代码事实】**

```
IMao-Launcher.exe            ← 固定启动入口（IMao-WinUI.exe 也会转交它）
  └─ IMao-WinUI.exe          ← WinUI 3 外壳，自包含 Windows App SDK
       └─ IMao-CoreHost.exe --pipe IMao.CoreHost.<WinUI的PID>.<32位GUID>
                             [--resource-snapshot <当前资源快照.json>]
```

- 管道由 **WinUI 创建客户端、CoreHost 创建服务端**；CoreHost 用
  `FILE_FLAG_FIRST_PIPE_INSTANCE` + 最大实例数 1，**一个进程只服务一个连接**（`CoreHostMain.cpp:861-869`）。
- WinUI 侧连接超时：**有资源快照时 180 秒**（`CoreHostService.cs:153`）。
- 应用**从不发送 `shutdown`**；关闭时直接断开管道，CoreHost 读到断开后自行 exit 0（`CoreHostMain.cpp:924-951`）。

### 2.2 `IMao-CoreHost.exe` 的全部模式 **【代码事实】**

| 命令行 | 需要 `--pipe` | 行为 | 退出码 |
|---|---|---|---|
| `--check-resources <资源根目录>` | **否** | 预载资源 + 初始化视觉/视口定位，stdout 打印**一行紧凑 JSON**，**不写任何日志文件** | 0 成功 / 1 失败 |
| `--check-resource-snapshot <快照.json>` | **否** | 同上，但先校验快照并用它的 `baselineRoot` | 0 / 1 |
| `--pipe <名字>`（正常模式） | — | 建管道 → **最多等 10 秒**客户端 | 0 正常关停 / **2 缺 `--pipe`** / 3 建管道失败 / 4 **10 秒无客户端** / 10-11 启动期崩溃 |
| `--resource-snapshot <路径>`（可与 `--pipe` 组合） | 否（但通常和它一起用） | 校验并应用快照 | 快照被拒 = 5 |

- **没有 `--help`、没有 `--version`**，未知参数被静默忽略。
- `--check-*` 模式要求 argv **恰好 3 个**，多一个参数就失效。

### 2.3 因此，以下判据是**错的**（本文档旧版如此写过，特此更正）

| 错误判据 | 为什么错 |
|---|---|
| "直接运行 `IMao-CoreHost.exe`，看进程是否驻留" | **【代码事实】** 不带 `--pipe` 会立刻 stderr 一行 + **exit 2**；带上 `--pipe` 而没有客户端，会在 **10 秒后自己退出，退出码 4**（`CoreHostMain.cpp:881-895`）。**它从不驻留。** |
| "看 `%LOCALAPPDATA%\IMao-WinUI\Logs\corehost-stderr.log` 有没有内容" | **【代码事实】** 这个文件的**唯一**写入者是 WinUI 的 `CoreHostService.cs:656`。**没起过 WinUI 就永远不会有这个文件**；而且 CoreHost 在正常管道模式下**根本不往 stderr 写东西**，所以即使起了 WinUI 它也大概率不存在。 |

---

## 3. 实机验证步骤（按成本从低到高）

> 每一步都写明：**命令 → 期望输出 → 判据 → 失败时收什么**。

### 第 0 步：原生核独立自检（不需要 UI、不需要游戏窗口、不需要客户端）

**【代码事实】** 这是唯一能"自己跑完就退出并给出机器可读结论"的模式。

```cmd
:: 0a. 最快：只验证资源根能不能加载
"<程序目录>\IMao-CoreHost.exe" --check-resources "<程序目录>\Assets"

:: 0b. 更接近真实启动：验证那份快照描述符（argv[2] 必须是绝对路径）
"<程序目录>\IMao-CoreHost.exe" --check-resource-snapshot "<绝对路径>\bundled-runtime.json"
```

- **期望输出**：stdout **一行**紧凑 JSON，形如
  `{"error":"","resourceSnapshotId":"...","resourcesReady":true,"viewportReady":true,"visualReady":true}`。
  **按字段名解析，不要按位置**（键按字母序输出）。
- **判据**：退出码 `0` **且** `resourcesReady == true` **且** `viewportReady == true`。
  真实消费者就是这么判的（`ResourceUpdateBootstrap.cs:129-139`、`ProgramUpdateStore.cs:615-620`）。
- **注意 1**：`--check-resources` 下 `resourceSnapshotId` 是字面量 `"bundled"`；只有 `--check-resource-snapshot`
  才会给出真快照 id。
- **注意 2**：该模式调用 `StructuredLogger::SetReadOnlyMode(true)`，**不写任何日志/崩溃/诊断文件**。
  结论只存在于 stdout，**必须自己把 stdout 重定向到文件**再回传。
- **注意 3**：程序自带的 `Assets/Updates/bundled-snapshot.json` 里的根是**相对路径**，**原生校验器拒绝相对根**
  （`scripts/Restore-BundledResources.ps1:17` 明确说明）。要用快照模式就得先把各根改写成绝对路径，
  仓库里的现成做法见该脚本第 13-25 行。
- **失败时收什么**：完整 stdout、退出码；以及 `%LOCALAPPDATA%\IMao-WinUI\CrashReports\corehost-*.json`（若存在）。

### 第 0' 步（可选）：用管道模式区分"参数/管道"与"资源"问题

```cmd
"<程序目录>\IMao-CoreHost.exe" --pipe imao-probe
```

- **期望**：约 **10 秒**后自行结束，**退出码 4**，stdout/stderr 无输出。
- **判据**：拿到 `4` 说明进程活着、参数正确、`CreateNamedPipeW` 成功，**卡在等客户端**（这是预期的）。
  其它码：`2` 参数不对；`3` 管道建不出来（重名或 Wine 不支持该组合）；`10/11` 启动期崩溃。
- **这一模式会写日志**：`%LOCALAPPDATA%\IMao-WinUI\Logs\events-<YYYYMMDD>.jsonl`，
  应含 `corehost-started`、`named-pipe-connect-failed`。
- **重要陷阱**：无客户端时资源预载会在关停时被取消，日志里很可能出现
  `resource-load-failed ... feature preload cancelled`。**这不能判为资源加载失败**——
  要判资源请用第 0 步。

### 第 1 步：捕获能力判定（决定这条路还要不要走下去）

**【代码事实】** `IMaoCaptureAssistant.exe` 是**本仓库自带的开发/校准工具**，由**仓库根** `CMakeLists.txt:272-280`
构建（源文件 `tools/CaptureAssistant/main.cpp` + `BitBltCapture.cpp`），产物在 `x64\<Config>\`。
**它不在发行包内**（发行包只含 `IMao-CoreHost.exe` 及其 DLL，见 `scripts/New-ProgramReleasePackage.ps1:66`）——
要用它必须先从构建产物里把 `x64\Release\IMaoCaptureAssistant.exe` 复制到 Deck 上。

```cmd
"<工具目录>\IMaoCaptureAssistant.exe" --scene Deck --sample 01 --output "<输出根>" --max-seconds 10
```

**【代码事实】** 参数与行为：

| 项 | 值 |
|---|---|
| 全部参数 | `--scene`（默认 `Unsorted`）、`--sample`（默认空→目录名用 `capture`）、`--output`（默认 `.\captures`）、`--max-seconds`（默认 10，范围 1–60）、`--help` |
| 名字约束 | `--scene`/`--sample` 只允许 `[A-Za-z0-9_-]`，长度 ≤ 80 |
| 未知参数 | **直接失败**（与 CoreHost 相反），exit 2 |
| 输出目录 | `<output>/<scene>/<sample 或 capture>_<YYYYMMDD-HHMMSS>/` |
| 输出文件 | `normal.png`（F9）、`map.png`（F10）、`move.mp4`（F11，15 fps H.264）、`capture-info.txt` |
| 热键 | **Ctrl+Alt+F9** 存正常画面、**Ctrl+Alt+F10** 存大地图、**Ctrl+Alt+F11** 开始/停止录像；`RegisterHotKey(hWnd=nullptr)` 线程级全局热键，**不需要本进程有焦点** |
| 目标窗口 | 可见、未最小化、可执行文件名 `Client-Win64-Shipping.exe`（大小写不敏感）、客户区 ≥640×360，取面积最大者 |
| 抓帧路径 | `PrintWindow(hwnd, dc, 3)`（`PW_CLIENTONLY｜PW_RENDERFULLCONTENT`），失败才回退 `BitBlt(SRCCOPY｜CAPTUREBLT)` |
| 不初始化 | OCR、特征匹配、叠加层——**确认未链接**这些 TU |
| 退出码 | 0 Ctrl+C 正常退出 / 2 参数错 / 3 输出目录建不了 / **4 三个热键中任一注册失败** / 5 COM 或 Media Foundation 起不来 |

- **期望**：启动即打印 `Capture helper is ready. Output: <目录>`；按 F9 后打印 `Saved normal screenshot: <完整路径>`。
- **判据（分两层，都必须做）**：
  1. **工具层**：进程能起来（排除 exit 4/5）、能打印就绪行、按 F9 能生成文件。
     **exit 4 是 Proton 下的关键信号**——`RegisterHotKey` 三连任一失败就整体退出。
  2. **内容层**：**必须用眼睛看 PNG**。
- **⚠️ 判断画面有效性的诚实结论** **【代码事实】**：程序**不区分**"实时帧 / 黑屏 / 上一次合成的残留帧"。
  没有哈希、没有帧计数写进文件、没有亮度/方差统计、PNG 里也没有时间戳元数据；`capture-info.txt` 只有
  会话级的 `createdAt`。**唯一的机器可读线索是"文件是否存在 + mtime"，内容只能靠肉眼判。**
- **建议的对照手法**：让游戏画面明显变化（开地图 / 关地图）后**再抓一张**，两张逐字节比较（`cmp`）；
  完全相同即高度可疑为残留帧。这是人工判据，不是程序判据。
- **失败时收什么**：完整控制台输出、退出码、输出目录内的文件清单、以及那两张 PNG 本身。

### 第 2 步：应用本体能否启动

这一节先给出**为什么预期起不来**，再给验证方法。**两条都不是已证实的运行结果**：

| 可疑阻塞 | 证据强度 | 证据 |
|---|---|---|
| WinUI 3 / Windows App SDK 的自包含运行时依赖 Windows 私有合成器组件 | **【合理推断】**（来自对已构建二进制的导入表取证，**未在 Proton 上实测**） | `IMao-WinUI.csproj:20` `WindowsAppSDKSelfContained=true`；`Microsoft.UI.Xaml.dll` 静态导入 `CoreMessagingXP.dll`、`Microsoft.Internal.FrameworkUdk.dll`，并经 `Microsoft.UI.Windowing.Core.dll` → `dcomp.dll` → `DwmSceneI.dll` / `dwmcorei.dll` |
| 清单要求管理员提升 | **【代码事实】** | `IMao-WinUI/app.manifest:20` `<requestedExecutionLevel level="requireAdministrator" uiData="false" />`；**【合理推断】** Steam / Gaming Mode 不会弹 UAC |

**【待实机】验证方法**：正常启动一次，然后看 `Logs/startup.log` 里出现的**第一个 `compat ` 行**与
**最后一个启动步骤标记**（`app-ctor-enter`、`app-host-ready`、`main-window-ctor` 等）。
两个标记都没有 = 死在加载器；有 `app-ctor-enter` 没有 `main-window-ctor` = 死在 App/窗口构造。
**`Logs/startup.log` 本身要求 WinUI 至少跑到写日志那一步**（`StartupLog` 在 App 构造函数第一行），
所以"完全没有这个文件"本身也是一条结论。

### 第 3 步：路径问题复现（本次修复的验收点）

1. **确认旧现象**（可选）：把安装目录放在一条软链下面，例如
   `mkdir -p /home/deck/real && ln -s /home/deck/real /home/deck/linked`，程序解压到 `/home/deck/linked/IMao`。
   旧版本会报 `资源目录不能包含符号链接或目录联接：Z:\home`（或任何位于安装目录上方的组件）。
2. **确认修复后可用**：同样布局下"检查更新"应正常返回，不再因为**上方**组件被拒绝。
3. **确认安全边界还在**：在**安装目录或更新目录内部**造一个 junction/软链，例如把
   `.../IMao/ProgramUpdates/versions` 指向别处，或把 `%LOCALAPPDATA%\IMao-WinUI\packages` 指向别处。
   **预期：仍然被拒绝**，且错误信息会写明"该链接在本程序受管理的目录之内"以及受管目录路径；
   被指向目录里的文件**必须一个都没少**。
4. **记录**：`Logs/startup.log` 里的 `compat path-mapping` / `compat link-refusal` 行。

### 第 4 步：更新事务

在有链接位于**上方**的安装里完整走一遍：检查更新 → 下载 → 解压 → 校验 → 重启启用 → 回滚。
判据：每一步都不报路径错误；`ProgramUpdates/versions/` 下出现新版本目录且校验通过。

---

## 4. Proton Prefix 配置（这一节不做，后面全部无意义）

### 4.1 为什么必须同一个 Wine 会话 **【合理推断】**

本程序需要做到三件事，而这三件事都要求两个进程处在**同一个 wineserver 实例**里：

| 需要 | 依赖的机制 | 为什么跨前缀不行 |
|---|---|---|
| 找到游戏窗口 | `EnumWindows`（`IMao-Core/src/util.cpp`，按 `Client-Win64-Shipping.exe` 匹配） | Wine 的窗口站/桌面是**按 wineserver 实例**隔离的；**推断**：不同前缀互相看不见对方的窗口 |
| 校验窗口身份 | `GetWindowThreadProcessId` / `IsWindow` | HWND 与 PID 都是会话内编号 |
| 与自己的 CoreHost 通信 | `\\.\pipe\IMao.CoreHost.<pid>.<guid>` | **推断**：Wine 的命名管道命名空间按前缀隔离（父子同前缀，所以这条本身没问题） |

**【待实机】** 以上三条中，第一条与第三条的具体行为必须实机确认；第二条是 Windows 语义，Wine 的实现差异未验证。

### 4.2 找到游戏的前缀

```bash
# 游戏的前缀目录（Steam 库里的 compatdata，按 AppID 命名）
ls -d ~/.local/share/Steam/steamapps/compatdata/*/pfx
# 逐个确认哪个里面有游戏：
grep -rl "Wuthering Waves\|Client-Win64-Shipping" ~/.local/share/Steam/steamapps/common/*/ 2>/dev/null | head
# 前缀内的盘符映射（c: 是 drive_c，z: 是整个 Linux 根）
ls -l ~/.local/share/Steam/steamapps/compatdata/<AppID>/pfx/dosdevices/
```

**【合理推断】** `dosdevices/` 里的 `c:` 指向该前缀自己的 `drive_c`，`z:` 指向 `/`，`s:` 通常是 Steam 库根。
程序看到的 `Z:\home\...` 就是这里的 `z:`。

### 4.3 让 IMAO 复用游戏的前缀

Steam 默认给每个"非 Steam 游戏"快捷方式**单独建一个 compatdata 前缀**。要让 IMAO 跑进游戏的前缀，
需要把 Proton 的兼容数据路径指过去。**下面这两个做法都属【待实机】——仓库里对这些选项名零命中，
本文不把它们写成既成事实。**

**做法 A：改快捷方式（推荐先试）**

1. 右键 IMAO 快捷方式 → 属性 → 兼容性 → 勾选"强制使用特定 Steam Play 兼容性工具"，
   选**与游戏相同的 Proton 版本**（菜单文案随 Steam 客户端版本变化）。
2. 在**启动选项**里前置环境变量，把兼容数据目录指向游戏的 compatdata：

   ```
   STEAM_COMPAT_DATA_PATH="/home/deck/.local/share/Steam/steamapps/compatdata/<游戏的AppID>" STEAM_COMPAT_CLIENT_INSTALL_PATH="/home/deck/.local/share/Steam" %command%
   ```

   **不确定点**：Steam 自己也会给非 Steam 快捷方式设 `STEAM_COMPAT_DATA_PATH`，
   启动选项里的赋值**是否稳定覆盖它，必须实机确认**——用 §4.4 第 0 步判。
3. 启动一次、退出，确认**没有**生成新的 `compatdata/<新AppID>` 目录。**新生成了就是没复用成功。**

**做法 B：直接用游戏的 Proton 跑（更朴素）**

```bash
STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
STEAM_COMPAT_DATA_PATH="$HOME/.local/share/Steam/steamapps/compatdata/<游戏的AppID>" \
"$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton" run \
  "<IMAO 安装目录>/IMao-WinUI.exe"
```

（`proton` 的确切路径与 `run` 子命令**需实机确认**，也可用第三方 `umu-launcher`。）

**一个必须知道的副作用** **【代码事实】**：一旦 IMAO 跑进游戏前缀，它的 `%LOCALAPPDATA%` 就变成
**游戏前缀**里的 `drive_c/users/<用户>/AppData/Local`（日志根 = `%LOCALAPPDATA%\IMao-WinUI`，
`IMao-Core/src/Runtime/StructuredLogger.cpp:45-51`）。**§6 的 `<前缀>` 从此指游戏的前缀。**

### 4.4 验证"确实在同一个会话"——五步，每步都有明确判据

**第 0 步（最低成本，不需要 IMAO 在跑，纯 Linux 侧）——前缀同一性**

```bash
# 游戏进程与 IMAO 进程的 PID
pgrep -f Client-Win64-Shipping
pgrep -f IMao
# 比较两者的 WINEPREFIX
tr '\0' '\n' < /proc/<游戏PID>/environ | grep -E 'WINEPREFIX|STEAM_COMPAT'
tr '\0' '\n' < /proc/<IMAO PID>/environ | grep -E 'WINEPREFIX|STEAM_COMPAT'
```

- **同一会话**：两者 `WINEPREFIX` **完全相同**（同一个 `…/pfx`）。
- **不同会话**：路径不同 ⟹ **必然失败，不必再往下查**。
- **为什么这条能代表窗口可见性** **【合理推断】**：一个前缀 = 一个 `wineserver`，
  窗口站 / 桌面 / 顶层窗口表 / 进程表 / `\\.\pipe\` 命名空间都由它持有。
  （**注意**：这条在 Windows 上不成立——`\\.\pipe\` 在 Windows 是机器级命名空间。是 Wine 让它变成前缀级的。）

**第 1 步（不需要 WinUI）——用 IMAO 自己的窗口搜索逻辑看游戏窗口**

桌面模式下跑 `IMaoCaptureAssistant.exe`（§3 第 1 步）。它用的是**同一套 `EnumWindows` 语义**。

- **同一会话**：打印 `Capture helper is ready. Output: …`，按 Ctrl+Alt+F9 得到
  `Saved normal screenshot: …`，且 PNG **不是黑屏**。
- **不同会话**：`Capture failed: Cannot find a visible Client-Win64-Shipping.exe window.`
- **中间态**：`The game window did not provide a usable frame. Keep the game visible and unminimized.`
  ⟹ 会话没问题，问题在捕获（DXVK / `PrintWindow`）。
- **坑**：它要注册 Ctrl+Alt+F9/F10/F11，**Gaming Mode 下可能被 Steam 抢走而直接 exit 4** ⟹ **用桌面模式跑**。

**第 2 步（不需要客户端）——原生核能不能在 Proton 下活**：见 §3 第 0 步。这一条**不涉及游戏窗口**，
只排除"核根本跑不起来"。

**第 3 步（要 WinUI 能起来）——应用自己的第一次判定**

点"开始探索"。**【代码事实】** 三种可区分的失败，**分别指向不同的进程**：

| 界面提示 | 含义 |
|---|---|
| InfoBar `Title=检查游戏窗口` / `Message=没有找到鸣潮的游戏窗口（可能已最小化或窗口太小）。…` | **WinUI 进程自己**没看到游戏窗口（`GameWindow.cs:97-98`）。它与 CoreHost 是两个进程，所以这条同样指向会话/可见性 |
| InfoBar `Title=正在等待游戏` / `Message=请先打开鸣潮，并回到可见的游戏画面。` + 状态"等待游戏窗口" | **CoreHost** 看不到游戏窗口（`CoreHostMain.cpp:688-694`，`coreState=waitingForGame`） |
| `叠加层启动失败，请回到可见的游戏窗口后重试` | 找到过但随后不可用（`DLL_API.cpp:238`） |

- 成功侧的证据：`Logs/events-*.jsonl` 里 `"message":"memory"` 且 `details` 含 `point=start-accepted`。

**第 4 / 5 步**：就是 §5 的第 4、5 步。第 5 步是最硬的证据——`returned-to-original-game`
（`GamepadWindowReturn.cs`）与 `activated-and-visible`（`GamepadWindowActivation.cs`）
**只有在** `SendMessageTimeout(game, WM_NULL, …)` 与 `AttachThreadInput(…, 游戏线程, …)`
真的对游戏窗口成功之后才可能出现。

**仓库里没有任何组件会 dump 一份"枚举到的窗口列表"** **【代码事实】**：
`IMaoCaptureAssistant` 只回答"找没找到"；开发探测工具 `IMaoOverlayPresentProbe.exe --mode=hittest`
会打印游戏 HWND（`IMao-Core/tools/OverlayPresentProbe/main.cpp:314`），但**它不在发行包**、
且其 README 要求管理员 PowerShell。要一份窗口清单只能自己写工具——**这一点已登记在
`Docs/OpenWork.md` §5b，作为"需要时再补"的项**。

### 4.5 如果做不到同前缀

**【合理推断】** 那本程序在 Proton 下**不可能**工作：找不到窗口 → 无法开始探索、无法定位、无法叠加。
这不是可以绕过的配置问题，而是架构前提。**遇到这种情况请直接停止实机验证并回报**，不要继续尝试按键。

---

## 5. 手柄：最简实机验证顺序

**架构不变**：复用现有 XInput 实现，**没有新增任何后端**。以下五步依次确认，**任一步失败就停在那一步收日志**。

| 步 | 确认什么 | 判据 | 失败时收什么 | 代码位置 |
|---|---|---|---|---|
| 1 | **XInput 枚举**：虚拟手柄是否出现在 0–3 槽位 | 设置页"使用的手柄"下拉里出现 `Xbox / XInput N` | 该下拉的截图 + 状态文字原文 + `gamepad-*.jsonl` | `ControllerDeviceSource.cs:57`（枚举 0–3）、`GamepadDevices.cs:40`（取**首个已连接**槽位，**不假定是 0**） |
| 2 | **后台读取**：IMAO 不聚焦时仍能读到 | 状态文字变为 `手柄 N 已连接 · 请先启动游戏叠加`（或 `请先松开按键、扳机并回正摇杆`） | `gamepad-*.jsonl` 中的 `input-state` 行 | `GamepadInputService.cs:184-232` |
| 3 | **游戏窗口识别**：核心是否找到游戏 | 状态文字不再是"等待核心返回最新地图状态"，进入地图/大世界提示 | `Logs/events-*.jsonl` 里 `"message":"memory"` 且 `details` 含 `point=start-no-game-window` 或 `point=start-accepted` | `IMao-Core/src/util.cpp:117-182`、`CoreHostMain.cpp:688-694` |
| 4 | **焦点判断**：前台窗口是否被认定为游戏 | 状态文字变成 `已连接 · LB 地图工具台 / RB 点位助手`（大地图）或大世界提示 | `gamepad-*.jsonl` 的 `input-state` 行——**关键是 `foreground=` 与 `gameHwnd=` 是否相等** | `GamepadInputService.cs:279` |
| 5 | **操作执行**：动作是否真的发生 | 按 LB 弹出地图工具台 | `gamepad-*.jsonl` 里 `returned-to-original-game` / `activated-and-visible` 等结果行 | `MapToolsController.cs`、`GamepadWindowReturn.cs`、`GamepadWindowActivation.cs` |

**第 4 步是分水岭**。**【代码事实】** `input-state` 行的两个值都是**十进制整数**：

```
{"timestamp":"…","severity":"info","category":"gamepad","message":"input-state",
 "details":"state=… device=… connected=… buttons=… lt=… rt=… lx=… ly=… rx=… ry=…
            axesNeutral=… tickGapMs=… contextAgeMs=… generation=… available=… bigMap=…
            gameHwnd=<十进制> foreground=<十进制>"}
```

- `gameHwnd=0` = 核心**没有**报出游戏窗口（字段缺失或非数字时按 0 处理）→ 第 3 步没过。
- `gameHwnd` 非 0 但 `foreground` 与之不相等 → 第 4 步没过，这正是 Proton/gamescope 下最可能碎的地方。
- **注意**：工具台/攻略路径另有一行**十六进制**的日志（`MapToolsController.cs` 的
  `map-tools-open-skipped foreground=0x… game=0x…`），**两处进制不同**，比对时不要混。

**状态文字会区分四种"检测不到"** **【代码事实】**（`GamepadInputService.cs:217-224`）：

| 文字 | 含义 |
|---|---|
| `未检测到 Xbox / PlayStation 手柄（XInput 槽位 0-3 均无设备；…）` | 输入组件正常，但没有任何设备 |
| `未检测到 Xbox / PlayStation 手柄 · XInput 组件不可用（…）` | 三个 XInput DLL 都加载不了——**是兼容层的问题，不是手柄的问题** |
| `未检测到已选择的 Xbox / XInput N…` / `未检测到已选择的原生 PS 手柄…` | 设置里指定的设备不在 |
| `请先松开按键、扳机并回正摇杆` | 设备已识别，等它回中位 |

### 5.1 Steam Input 配置 **【合理推断，待实机】**

1. 在 Steam 里把 IMAO 添加为非 Steam 游戏快捷方式。
2. 属性 → 控制器：**不要**禁用 Steam 输入；布局选把内置手柄呈现为 **Xbox 手柄**的模板。
3. 背键 **L4/L5/R4/R5** 本次不适配，请在 Steam Input 里自行映射到 A/B/X/Y、LB/RB。
4. **必须在启动 IMAO 之前配好**，并与游戏同一前缀（§4）。

**为什么不会被读两遍** **【代码事实】**：`ControllerDeviceSource.cs:84` 的
`GetGamepadTypeForId(instance) is not (5 or 6) → continue` 让 SDL3 只接管 PS4/PS5；
Xbox 型走 XInput 独占 handle 0–3。**【合理推断】** 若 Steam Input 模板把内置手柄呈现为 DualSense（类型 5/6），
SDL 会另外打开同一只物理手柄——两者最终汇入同一个状态机，**不会重复触发动作**，但提示布局可能错配。

### 5.2 按键语义（沿用现有 Xbox 布局，没有新增模式）

大地图 LB→工具台、RB→点位助手；探索中 LB+B→完成附近点位、LB+X→附近攻略；LB+Start→启停探索；
独立攻略内 LS→切换焦点；A/B→确认/返回；按住 A 0.6 秒完成、按住 Y 0.6 秒跳过、列表页按住 X 全收；
攻略内 LB/RB 翻图、右摇杆滚动、X 放大图片、LT/RT 缩放。

---

## 6. 日志收集（含每个文件的**产生前提**）

**【代码事实】** 目录都是 `%LOCALAPPDATA%\IMao-WinUI\`。在 Proton 下即
`<前缀>/drive_c/users/<用户>/AppData/Local/IMao-WinUI/`，其中 **`<前缀>` 是 IMAO 实际运行的那个前缀**——
按 §4.3 配好之后它就是**游戏的前缀**，而不是 Steam 给快捷方式单独建的那个。

| 文件 | 产生前提 | 内容 |
|---|---|---|
| `Logs\startup.log` | **WinUI 起来过**（`StartupLog` 在 App 构造函数里写） | 启动步骤标记、`compat ` 路径诊断 |
| `Logs\events-<日期>.jsonl` | **CoreHost 以管道模式起过**（自检模式写只读、不落盘） | 原生事件：`corehost-started`、`named-pipe-connect-failed`、`resource-load*` 等 |
| `Logs\corehost-stderr.log` | **WinUI 起过，且 CoreHost 往 stderr 写了非空行** | 子进程 stderr（含匹配器异常的说明） |
| `Logs\gamepad-<日期>.jsonl` | **WinUI 起过且手柄服务在跑** | 手柄事件，JSON 行：`timestamp/severity/category/message/details` |
| `CrashReports\corehost-<日期>-<tick>.json` | CoreHost 未捕获异常时 | 崩溃报告 |
| `Diagnostics\<时间戳>\` | **仅当诊断截图被开启** | 诊断截图会话 |

**换句话说：`corehost-stderr.log` 和 `startup.log` 都要求 WinUI 曾经启动过。**
只跑过 `--check-resources` 的机器上，**这些文件都不会存在**——那时唯一的证据是**你自己重定向的 stdout**。

**【待实机】** `%LOCALAPPDATA%` 在具体前缀里解析到哪个目录，代码给不出答案（代码只读环境变量，
读不到时回落到 `%TEMP%\IMao-WinUI`）。用 `find` 定位是最稳的：

```bash
find ~/.steam ~/.local/share/Steam -name "startup.log" -o -name "events-*.jsonl" 2>/dev/null
```

**上报时请一并给出**：上表里能拿到的文件、Proton 版本、Steam Input 布局名称、
以及 §3/§5 每一步的判据原文。

---

## 7. 尚需实机回答的问题

1. `Microsoft.UI.Xaml.dll` 在具体 Proton 版本上是否真的加载失败（【合理推断】它需要
   `CoreMessagingXP.dll`、`Microsoft.Internal.FrameworkUdk.dll`、`dcomp.dll` → `DwmSceneI/dwmcorei`）。
2. `requireAdministrator`（`IMao-WinUI/app.manifest:20`）在 Steam 启动路径下是否直接静默退出。
3. CoreHost 能否活过 `CreateNamedPipeW(FILE_FLAG_FIRST_PIPE_INSTANCE｜FILE_FLAG_OVERLAPPED｜PIPE_REJECT_REMOTE_CLIENTS)`，
   以及 10 秒超时是否按预期触发 exit 4。
4. `--check-resources` 能否在 Proton 下走完资源预载（这是**不需要 UI 的最有信息量的一步**）。
5. `IMaoCaptureAssistant` 的 `RegisterHotKey` 三连是否全部成功（失败即 exit 4）。
6. `PrintWindow(PW_RENDERFULLCONTENT)` 对 DXVK 窗口返回的是否是**当前帧**（不是黑屏、不是残留帧）。
7. Wine 的 `xinput1_4.dll` 回退链是否存在并能读到 Steam Input 的虚拟手柄。
8. **`GetForegroundWindow()` 是否等于核心记录的游戏 HWND**（Gaming Mode / gamescope 下）。
9. `AttachThreadInput`/`SetForegroundWindow`、`SendMessageTimeout(WM_NULL, …, 100ms)`、
   `WindowFromPoint` 五点遮挡采样在 Wine 下的行为。
10. IMAO 与游戏能否通过 §4.3 的配置真正进入同一 Wine 会话。

---

## 8. 验证状态标记

| 项目 | 状态 | 判据 |
|---|---|---|
| 自动化测试通过（路径边界） | **通过** | `scripts/Test-ProgramUpdates.ps1`：66 项；`scripts/Test-ResourceUpdates.ps1`：资源更新 126 项、发布器 34 项 |
| 自动化测试通过（手柄状态机 / 运行时） | **通过** | `scripts/Test-Runtime.ps1 -SkipBuild`（含 ControllerRuntime、ManagedRuntime 等） |
| Windows x64 构建 | **通过** | `dotnet build IMao-WinUI/IMao-WinUI.csproj -c Release -p:Platform=x64 -r win-x64` → 0 error |
| **Proton 环境验证** | **未进行** | 入口是 §3 第 0 步（不需要 UI） |
| **Steam Deck 实机验证** | **未进行** | 入口是 §4（前缀）→ §3 第 0/1 步 |

**不得把第一栏当作第三栏。** 自动化里的"兼容层路径"是用 NTFS 目录联接（junction）**模拟**出来的：
它复现的是"受管目录上方的组件是重解析点"这一几何关系，**不是 Proton 本身的行为**。
