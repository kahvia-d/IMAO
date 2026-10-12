# IMAO 在 Steam Deck / Proton 上的验证与使用指南

> 面向：手上有 Steam Deck 的玩家与维护者。
> 本文区分三种结论，**不得互相替代**：**自动化测试通过**（仓库内模拟，本机可复现）、
> **Proton 环境验证通过**（在 Linux + Proton 上真的跑起来过）、**Steam Deck 实机验证通过**（在掌机上真的跑起来过）。
> 截至本文写作时，只有第一项成立；第二、三项尚未进行。

---

## 0. 先说结论：当前版本能不能在 Steam Deck 上跑

**不能启动，而且原因不在本次修复的范围里。** 这一步必须先说清楚，否则后面的按键验证无从谈起。

本次改动（路径安全边界 + 输入诊断）解决的是**启动之后**的两类问题：资源/程序更新被路径检查拦住，以及
手柄读不到时说不清原因。它**不解决**下面这两条启动级阻塞：

| 阻塞 | 证据 | 性质 |
|---|---|---|
| WinUI 3 / Windows App SDK 的自包含运行时依赖 Windows 私有合成器组件 | `IMao-WinUI/IMao-WinUI.csproj:20` `WindowsAppSDKSelfContained=true`；`Microsoft.UI.Xaml.dll` 静态导入 `CoreMessagingXP.dll`、`Microsoft.Internal.FrameworkUdk.dll`，并经 `Microsoft.UI.Windowing.Core.dll` → `dcomp.dll` → `DwmSceneI.dll` / `dwmcorei.dll` 拉进一整套 DWM 合成器内核分叉 | **高置信度推断**（来自导入表取证，未在 Proton 上实测） |
| 清单要求管理员提升 | `IMao-WinUI/app.manifest:20` `<requestedExecutionLevel level="requireAdministrator" uiData="false" />` | **代码事实**。Steam / Gaming Mode 不会弹 UAC，Wine 也无法在非交互会话里提升 |

**换句话说：路径与输入即使完全正确，玩家现在也看不到窗口。** 这一条属于产品级决策（换 UI 外壳 / 改清单），
不是兼容性补丁，因此本次没有改动，只做记录。

### 0.1 如果启动阻塞被解决，后面还会遇到什么

| 能力 | 依赖 | Proton 下预期 | 置信度 |
|---|---|---|---|
| 游戏窗口定位 | `EnumWindows` + 进程名匹配，需同一 Wine 会话 | 同 prefix 可用；**跨 prefix 必然失败** | 高 |
| 画面捕获 | 默认 `Windows.Graphics.Capture`，失败自动回落 BitBlt | WGC 不可用；`BitBlt`/`PrintWindow` 对 DXVK 渲染窗口大概率拿不到画面 | 中高 |
| 叠加层 | 默认 DirectComposition，失败自动回落 layered colorkey | 合成路径可能不可用；colorkey 路径有机会 | 中 |
| 命名管道 IPC（WinUI ↔ CoreHost） | Wine 命名管道，同 prefix | 可用 | 高 |
| XInput 手柄 | Wine 的 `xinput1_4` 等 | 有机会可用，见第 3 节 | 中 |
| SDL3 原生 PS 手柄 | hidraw 原始 HID | 不可用 | 中 |

---

## 1. 本次真正修好的东西：路径安全边界

### 1.1 玩家报的现象

```
资源目录不能包含符号链接或目录联接：Z:\home
```

### 1.2 根因

`RejectLink()`（`IMao-WinUI.Core/Updates/UpdateStorage.cs`）原来从给定路径**逐级检查到卷根**。
Wine 只对**路径中那一段本身是符号链接**的组件报 `FILE_ATTRIBUTE_REPARSE_POINT`，而 Steam Deck 上
这类组件是**平台自己造的**，且都在本程序目录的**上方**：

- `~/.steam/steam` → `~/.local/share/Steam`
- 不可变根发行版（如 Bazzite）把 `/home` 做成指向 `/var/home` 的符号链接
- Wine 的 `Z:` 盘映射本身

于是"本程序自己放的每一个目录都是真实目录"的安装，仍然会被上方的平台链接判死。报错里的 `Z:\home`
正是这样一个**位于安装目录之上**的组件。

### 1.3 修法

引入**受管根目录（owned root）**作为检查边界：

- 每个调用点声明自己拥有的目录——程序更新用安装根，资源更新用更新根，程序自带资源用 `Assets` 根。
- 检查从目标路径向上走到该边界为止，**边界本身也在检查范围内**（边界是链接照样拒绝）。
- 边界**上方**的链接只写一条诊断，不参与判定。
- 没有声明边界的调用点（发布器等本机工具）保持原来的"查到卷根"行为。
- **没有任何操作系统判断，也没有任何路径白名单**：`Z:`、`Z:\home`、含 `home` 的路径都没有被特判。

### 1.4 为什么这不削弱安全

- 真正危险的是**受管目录内部**的链接：那里会发生解压、版本切换、回滚和递归删除。边界内一律照旧拒绝。
- 边界上方的链接无法把边界内的路径重定向出去：那些组件对所有路径都是**共同前缀**，解析前后包含关系不变
  （`SafeChild` 依赖的正是这一点）。
- 玩家把安装目录本身放在链接下面，攻击者拿不到新能力：程序就是从那条路径启动的，能改这条链接的人本来就能换掉可执行文件。
- 解压路径逃逸、恶意 ZIP 条目、临时目录清理越界这些既有防线一条都没有动，测试也仍然钉着它们。

### 1.5 诊断日志

`%LOCALAPPDATA%\IMao-WinUI\Logs\startup.log` 里会出现以 `compat ` 开头的行，三种类别：

| 类别 | 含义 | 玩家的下一步 |
|---|---|---|
| `path-mapping` | 受管目录**上方**有链接（兼容层盘符映射、Steam 库软链、玩家自建 junction）。**不影响功能** | 无需处理，仅用于确认环境形状 |
| `link-refusal` | 受管目录**内部**有链接，已被拒绝 | 删除该链接，或把安装/更新目录移到没有链接的位置 |
| `path-inaccessible` | 路径存在但读不到属性（权限、盘符映射问题） | 检查权限与盘符，不是安全判定 |

每类最多 64 行、按内容去重，**不含任何凭据**。

---

## 2. 实机验证步骤（按成本从低到高，做到哪一步就记到哪一步）

### 第 0 步：先跑原生核，不碰 UI（信息量最高、成本最低）

```bash
# 在 Steam Deck 桌面模式下，先确认游戏的 Proton 前缀
ls ~/.steam/steam/steamapps/compatdata/
```

- 用 Proton 直接运行 `IMao-CoreHost.exe`（它只依赖 Win32/D3D11/DXGI/命名管道，是整套里最有可能在 Wine 上活下来的部分）。
- **判据**：进程是否驻留、`%LOCALAPPDATA%\IMao-WinUI\Logs\corehost-stderr.log` 是否有内容。
- 如果这一步就活不下来，后面所有验证都没有意义。

### 第 1 步：判定"能不能看到游戏画面"

- 单独运行 `IMaoCaptureAssistant.exe`（`IMao-Core/CMakeLists.txt` 里的独立捕获助手，不初始化识别与叠加层）。
- **判据**：它导出的画面是不是黑的。**如果是黑的，整个方向在 Proton 上就没有意义**，不必再做第 2 步。

### 第 2 步：应用本体能否启动

- 见第 0 节的表。**预期：目前不能启动。**
- 如果将来能启动，请记录 `Logs/startup.log` 里出现的第一个 `compat ` 行与最后一个启动步骤标记
  （`app-ctor-enter`、`app-host-ready`、`main-window-ctor` 等），这三条一起就能定位卡在哪一步。

### 第 3 步：路径问题复现（本次修复的验收点）

1. **确认修复前的现象**（可选，用于确认根因）：把安装目录放在一条软链下面，例如
   `mkdir -p /home/deck/real && ln -s /home/deck/real /home/deck/linked`，把 IMAO 解压到
   `/home/deck/linked/IMao`。旧版本会在 `Logs/startup.log` 或界面报
   `资源目录不能包含符号链接或目录联接：Z:\home`（或任何位于安装目录上方的组件）。
2. **确认修复后可用**：同样布局下，资源更新的"检查更新"应当正常返回结果，不再因为
   `Z:\home` 这类**上方**组件被拒绝。
3. **确认安全边界还在**：在**安装目录或更新目录内部**造一个 junction/软链，例如把
   `.../IMao/ProgramUpdates/versions` 指向别处，或在资源目录里把 `packages` 指向别处。
   **预期：仍然被拒绝**，错误信息里会写明"该链接在本程序受管理的目录之内"以及受管目录的路径。
   被指向的目录里的文件**必须一个都没少**。
4. 记录：`Logs/startup.log` 里的 `compat path-mapping` / `compat link-refusal` 行。

### 第 4 步：更新事务本身

- 在**有链接在上方**的安装里完整走一遍：检查更新 → 下载 → 解压 → 校验 → 重启启用 → 回滚。
- **判据**：每一步都不报路径错误；`ProgramUpdates/versions/` 下出现新版本目录且校验通过。

---

## 3. Steam Input 与手柄

### 3.1 Steam Input 配置

1. 在 Steam 里把 IMAO 添加为**非 Steam 游戏**快捷方式。
2. 右键该快捷方式 → 属性 → 控制器：
   - **禁用 Steam 输入**：否（必须让 Steam Input 介入）。
   - 布局模板：选择把内置手柄呈现为 **Xbox 手柄**的模板（"手柄"类模板，而非"手柄 + 触控板"之外的其它呈现）。
     - **为什么**：IMAO 走的是 `XInputGetState`，并且**有意只用这一个入口**
       （`IMao-WinUI/Services/XInputControllerReader.cs`，无 `XInputGetCapabilities`、无 `XInputGetKeystroke`、
       无振动）。Steam Input 把它呈现成 Xbox 手柄后，它在 Wine 里就是 slot 0 的一只 XInput 设备。
   - 背键 **L4 / L5 / R4 / R5**：本次**不直接适配**，请在 Steam Input 里自行映射到 A/B/X/Y、LB/RB 等已有按键。
3. **必须在启动 IMAO 之前配置好**，并让 IMAO 与游戏运行在**同一个 Proton 前缀**内。

### 3.2 为什么不会和 SDL3 抢同一只手柄

`IMao-WinUI/Services/ControllerDeviceSource.cs` 里的 SDL3 路径**只接管类型 5/6（PS4/PS5）**：

```csharp
if (SdlNative.GetGamepadTypeForId(instance) is not (5 or 6)) continue;
```

Xbox 型（类型 2/3）会被直接跳过，XInput 路径独占 handle 0-3。所以 Steam Input 呈现成 Xbox 手柄时，
同一只虚拟手柄**不会**被两个后端各读一遍，也不会重复触发动作。

### 3.3 按键语义（沿用现有 Xbox 布局，没有新增模式）

| 场景 | 按键 | 动作 |
|---|---|---|
| 大地图 | LB | 打开地图工具台 |
| 大地图 | RB | 打开独立点位助手 |
| 探索中 | LB 然后 B | 完成附近点位（全部松开后执行一次） |
| 探索中 | LB 然后 X | 打开附近攻略 |
| 任意 | LB + Start | 开始 / 停止探索（在输入状态机之外） |
| 独立攻略内 | LS | 在攻略与游戏之间切换焦点 |
| 通用 | A / B | 确认 / 返回 |
| 详情页 | 按住 A 0.6 秒 | 完成当前点 |
| 详情页 | 按住 Y 0.6 秒 | 跳过当前导航目标 |
| 列表页 | 按住 X | 收下附近列表全部点位 |
| 攻略内 | LB / RB | 上一张 / 下一张 |
| 图片页 | X / LT / RT | 放大图片 / 缩小 / 放大 |

Steam Deck 的物理位置一一对应：A/B/X/Y、L1/R1→LB/RB、L2/R2→LT/RT、摇杆按下→LS/RS、View/Menu→Back/Start。

### 3.4 按键验证方法

1. 设置页 → "Xbox / PlayStation 手柄" → 打开"启用手柄操作"，点"刷新手柄列表"。
2. **判据 A（读得到）**：状态下拉里出现 `Xbox / XInput 1`，或状态文字变成
   `手柄 1 已连接 · 请先启动游戏叠加`。
3. **判据 B（动作发得出）**：打开游戏大地图，按 LB，地图工具台应当出现。
4. **如果只满足 A 不满足 B**：这是"读得到手柄但拿不到前台"。请看第 3.5 节的四种状态区分，
   并把 `Logs/gamepad-*.jsonl` 里的 `input-state` 行（含 `foreground=` 与 `gameHwnd=` 两个十六进制值）
   提供给维护者——**这两个值是否相等，是实机上唯一能给出答案的问题**。

### 3.5 检测不到手柄时，状态文字会区分四种情况

| 状态文字 | 含义 | 下一步 |
|---|---|---|
| `未检测到 Xbox / PlayStation 手柄（XInput 槽位 0-3 均无设备；掌机内置手柄需先由 Steam Input 呈现为 Xbox 手柄）` | 输入组件正常，但没有任何设备 | 检查 Steam Input 布局与手柄连接 |
| `未检测到 Xbox / PlayStation 手柄 · XInput 组件不可用（…）` | 三个 XInput DLL（`xinput1_4` / `xinput1_3` / `xinput9_1_0`）在兼容层里都加载不了 | **这是兼容层的问题，不是手柄的问题**；换 Proton 版本或上报 |
| `未检测到已选择的 Xbox / XInput N…` / `未检测到已选择的原生 PS 手柄…` | 玩家在设置里指定了具体设备，但那个设备不在 | 改回"自动选择"，或重新连接该手柄 |
| `请先松开按键、扳机并回正摇杆` | 设备已识别，正在等它回到中位 | 松开所有按键、扳机，摇杆回中 |
| `手柄 N 已连接 · 请先启动游戏叠加` | 设备已识别，核心没有在运行 | 启动游戏叠加 |
| `等待核心返回最新地图状态` / `<核心返回的画面提示>` | 设备与核心都就绪，但游戏画面状态不可确认 | 回到游戏大地图 |
| `请切回游戏后操作` | 设备已识别，但当前前台窗口不是游戏窗口 | 这是焦点条件不满足，见上 |

---

## 4. 日志收集

| 文件 | 位置（Proton 下） | 内容 |
|---|---|---|
| `startup.log` | `<prefix>/drive_c/users/steamuser/AppData/Local/IMao-WinUI/Logs/startup.log` | 启动步骤标记、`compat ` 路径诊断 |
| `gamepad-YYYYMMDD.jsonl` | 同上目录 | 手柄事件（每行一个 JSON：`timestamp/severity/category/message/details`） |
| `corehost-stderr.log` | 同上目录 | 原生核的标准错误 |
| `resource-updates.log` | 同上目录 | 资源更新事务 |

`<prefix>` 通常是 `~/.steam/steam/steamapps/compatdata/<AppID>/pfx`。快捷方式的 AppID 可在
Steam 的 `userdata/<id>/config/shortcuts.vdf` 里找到，或直接用 `find ~/.steam -name startup.log -newer /tmp/marker` 定位。

应用内路径：设置页/诊断页都有"打开日志"入口（`IMao-WinUI/Views/DiagnosticsPage.xaml.cs`）。

**上报时请一并给出**：`startup.log` 的 `compat` 行、`gamepad-*.jsonl` 里最新的 `input-state` 行、Proton 版本号、
Steam Input 所用布局名称。

---

## 5. 尚需实机回答的问题（代码给不出答案）

1. `Microsoft.UI.Xaml.dll` 在具体 Proton 版本上是否真的加载失败（导入表证明它**需要**那些私有 DLL，能否跑通要看内部实现）。
2. 清单里的 `requireAdministrator`（`IMao-WinUI/app.manifest:20`）在 Steam 启动路径下是否直接导致静默退出。
3. Wine 的 `xinput1_4.dll`（或其回退链）是否存在，且能读到 Steam Input 的虚拟手柄。
4. `GetForegroundWindow()` 在 Gaming Mode / gamescope 全屏下是否**等于**核心记录的游戏 HWND
   （`IMao-WinUI/Services/GamepadInputService.cs` 的前台判定）。
5. `AttachThreadInput` + `SetForegroundWindow` 在 Proton 下能否把前台交给本程序自己的窗口
   （`IMao-WinUI/Services/GamepadWindowActivation.cs`）。
6. `SendMessageTimeout(WM_NULL, BLOCK|ABORTIFHUNG|ERRORONEXIT, 100ms)` 在 Wine 下的返回值。
7. `WindowFromPoint` 五点遮挡采样在 gamescope 全屏下是否会把窗口判成不可用
   （`IMao-WinUI/Services/GamepadWindowVisibility.cs`）。
8. `BitBlt` / `PrintWindow` 能否从 DXVK 渲染的游戏窗口拿到**正确的**画面（不只是"非黑"，还要不是上一次合成的残留帧）。
9. 叠加层窗口在 gamescope 下的 z 序与命中测试行为。
10. XInput 回退链里的其它 DLL 名称是否都存在于目标 Proton 版本。

---

## 6. 验证状态标记

| 项目 | 状态 | 判据 |
|---|---|---|
| 自动化测试通过（路径边界） | **通过** | `scripts/Test-ProgramUpdates.ps1`：65 项；`scripts/Test-ResourceUpdates.ps1`：资源更新 124 项、发布器 34 项 |
| 自动化测试通过（手柄状态机） | **通过** | `Tests/ControllerRuntime`、`Tests/MapToolsRuntime`、`Tests/GuideWindowRuntime` 等既有套件 |
| Windows x64 构建 | **通过** | `dotnet build IMao-WinUI/IMao-WinUI.csproj -c Release -p:Platform=x64 -r win-x64` |
| **Proton 环境验证** | **未进行** | 需要有 Linux + Proton 的机器；第 0–2 步是入口 |
| **Steam Deck 实机验证** | **未进行** | 需要掌机；第 0–4 步是入口 |

**不得把第一栏当作第三栏。** 自动化测试里的"兼容层路径"是用 NTFS 目录联接（junction）
**模拟**出来的：它复现的是"受管目录上方的组件是重解析点"这一几何关系，不是 Proton 本身的行为。
