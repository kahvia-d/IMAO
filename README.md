<!-- markdownlint-disable MD033 MD041 -->

<div align="center">

<img alt="LOGO" src="Docs/images/readme/logo.png" width="148" height="148" />

# IMao · 鸣潮地图工具

<br>

<div>
    <img alt="platform" src="https://img.shields.io/badge/platform-Windows%20x64-blueviolet">
    <img alt="license" src="https://img.shields.io/github/license/kahvia-d/IMAO">
</div>
<div>
    <img alt="GitHub release" src="https://img.shields.io/github/v/release/kahvia-d/IMAO?color=%2343e28a">
    <img alt="GitHub all releases" src="https://img.shields.io/github/downloads/kahvia-d/IMAO/total?style=social">
    <img alt="GitHub Repo stars" src="https://img.shields.io/github/stars/kahvia-d/IMAO?style=social">
</div>
<br>

[简体中文](README.md) | [English](README.en.md)

**把库街区大地图直接叠在游戏窗口上。**

IMAO 是免费、开源、非官方的《鸣潮》社区地图辅助工具。

点位、路线、收集进度都画在游戏画面里，探索时不必再反复按 <kbd>Alt</kbd>+<kbd>Tab</kbd> 切来切去。

绝赞更新中 ✿✿ヽ(°▽°)ノ✿

</div>

<img alt="主界面" src="Docs/images/readme/app-overview.png" />

## 下载与安装

前往 **[Releases](https://github.com/kahvia-d/IMAO/releases/latest)** 下载 **`IMao-v<版本号>-windows-x64.zip`**，
解压后运行 **`IMao-Launcher.exe`** 即可。

> 下载页里 `ext-` 开头的是**库街区同步浏览器扩展**，不是程序包；程序包是 `IMao-v…-windows-x64.zip`。

### 下载渠道

- **[GitHub Releases](https://github.com/kahvia-d/IMAO/releases/latest)**：本项目的正式发布页，提供完整安装包免费下载，也是程序内更新源。
- **QQ 群文件**：用户交流群 **`1109700733`** 提供完整安装包免费下载，主要作为国内用户的备用免费渠道；需人工下载完整包，不是程序内自动更新源。
- **[Mirror酱](https://mirrorchyan.com/zh/projects?rid=IMAO&source=imao_app_settings)**：可选的第三方高速下载与更新服务，需要 Mirror酱 CDK，也是程序内可选更新源。

IMAO 本身永久免费，没有付费版、会员版或功能解锁。Mirror酱的付费服务对应网络分发与更新服务，
**不会解锁任何额外 IMAO 功能**；不使用 Mirror酱也不会影响 IMAO 的正常功能，仍可通过 GitHub Releases 或 QQ 群文件免费获取软件。

| 项目 | 要求 |
| :--- | :--- |
| 系统 | Windows 10 1809（17763）或更高，**仅支持 x64** |
| 游戏画面 | 任意比例的客户区（16:9 / 16:10 / 21:9 都可以）；坐标识别按游戏自身的 HUD 缩放定位，不再限制具体分辨率 |
| 运行库 | **无需安装 .NET**（启动器自包含）；VC++ 运行库随包附带 |
| 磁盘 | 完整包解压后约 **1.5 GB**（已含全部地区地图资源） |

> 更新也很快：程序内「设置 → 检查更新」只会下载**与本机不一致的分片**，只改点位时通常在 1.6 MB 左右，
> 常规程序更新约 30 MB，不需要每次重下整个包。

## 亮点功能

- **实时同步位置**：小地图特征匹配为主，辅以游戏左下角坐标识别与大地图视口解算，把玩家位置实时画在地图上；
  短暂跟丢时按轨迹外推继续移动（外推 1 秒误差约 7 个单位）。
- **交互式大地图 / 小地图导航**：覆盖层直接画在游戏画面上，支持点位图标、数量角标、重叠点位展开、
  已完成点显示与**点位筛选**，主界面与游戏内工具台共用同一份筛选状态。
- **自动路线规划**：按平面距离做最近邻 + 2-opt 局部搜索，单条路线最多 **500 个目标**；
  实测 500 点规模下求解 P95 约 **13.7 ms**，站定不动时也不会卡。支持预览、保存、载入与**实时重排剩余路线**。
- **附近点位一键收集**：默认按小地图 15 像素范围（5–120 可调）判定，只有图标相互遮挡时才列出候选；
  候选列表可连续处理，也能「一键收集本组全部点位」。
- **攻略浮窗**：点位详情、图文攻略、翻页与缩放，大图为独立置顶窗口；攻略内容在线缓存，**查看公开攻略无需登录**。
- **分层地图自动识别**：进入洞穴、地下建筑后自动只显示所在楼层点位，其它楼层以暗色图标加「上／下」角标区分，
  地表点位自动隐藏；走出分层区域立即恢复。目前覆盖 **57 个分层地图、90 个楼层**。
- **地区按需下载**：每个地区是一份独立的特征包，用不到的地区可以停用或直接从磁盘删除腾空间，
  之后随时再点「下载」取回。支持**导入离线包**（`resources-<版本>-offline.zip`）。
- **Xbox / PlayStation 手柄支持**：打开工具台、点位助手、完成附近点位、一键收集、缩放攻略图，都可以不碰键盘。
- **库街区进度同步**：配合[浏览器扩展](https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg)，
  把本机收集记录与自己的库街区账号双向同步：首次合并采用并集，后续也会同步用户取消的完成状态。
- **安全的更新链路**：清单经 ECDSA P-256 签名，逐包校验 SHA-256，禁止包内出现可执行文件；
  更新只动程序与地图资源，**绝不覆盖**你的完成记录、路线、筛选与个人设置。
- **可选的下载源**：设置 →「下载源管理」二选一——**GitHub**（免费，国内常常连不上），或
  [Mirror酱](https://mirrorchyan.com/zh/projects?rid=IMAO&source=imao_app_settings)（可选第三方高速下载与更新服务，需要 Mirror酱 CDK，不解锁额外功能）。
  选定后**检查更新和下载更新都只走它**。

<!-- markdownlint-disable -->

<details>
<summary><b>功能演示</b>（点击展开，共 5 张）</summary>

<br>

**小地图导航** —— 探索时点位就在小地图上，路线与数量角标一并显示。

<img alt="小地图导航" src="Docs/images/readme/minimap-navigation.jpg" />

**小地图细节** —— 图标重叠时显示数量角标，快捷键可在范围内就近完成收集。

<img alt="小地图细节" src="Docs/images/readme/minimap-markers.jpg" />

**交互式大地图** —— 与游戏大地图完全对位，左键看攻略、右键切换完成状态。

<img alt="交互式大地图" src="Docs/images/readme/map-markers.jpg" />

**路线指引** —— 规划完成后按顺序指引，靠近目标自动切换下一站。

<img alt="路线指引" src="Docs/images/readme/map-route.jpg" />

**路径自动规划** —— 矩形框选 / 自由套索 / 加入当前视野，生成预览后一键开始指引。

<img alt="路径自动规划" src="Docs/images/readme/map-plan.jpg" />

</details>

<!-- markdownlint-restore -->

## 使用说明

### 快速开始

1. 解压下载的压缩包，运行 `IMao-Launcher.exe`。
2. 打开鸣潮（客户区 16:9 / 16:10 / 21:9 都可以），并让**左下角的坐标读数清晰显示**（这是工具的定位来源之一）。
3. 回到工具点「开始探索」，等右下角的状态提示消失，说明已成功定位。
4. **冷启动时请先打开一次游戏大地图**，让工具确定你所在的区域；在此之前状态栏会提示
   「请打开一次大地图以确定所在区域」。之后进出任何地区都会自动识别，不用再手动切换。
5. 打开设置页按需开启「小地图点位 / 大地图点位 / 状态条」，或先下载要用的地区资源。

### 默认快捷键

在**设置 → 键盘快捷键**里可以逐项修改、禁用或恢复默认，保存后立即生效。

| 按键 | 作用 |
| :--- | :--- |
| <kbd>Z</kbd> | 完成附近点位（范围内多个点位图标重叠时先弹出候选列表） |
| <kbd>F8</kbd> | 打开 / 关闭当前目标攻略 |
| <kbd>Q</kbd> | 在大地图上记录手绘路线的两个端点 |
| <kbd>PageUp</kbd> / <kbd>PageDown</kbd> | 攻略图上一张 / 下一张 |
| <kbd>F9</kbd> | 开始 / 停止探索（与首页按钮等价） |
| <kbd>Shift</kbd> + 左键拖动 | 自动路线选点时的临时矩形框选 |
| <kbd>Esc</kbd> | 取消当前手势，否则回到移动地图 |

> <kbd>M</kbd>、<kbd>F10</kbd>、<kbd>Esc</kbd> 为程序保留键，不可绑定；<kbd>Ctrl</kbd>/<kbd>Alt</kbd>/<kbd>Shift</kbd>/<kbd>Win</kbd>
> 组合键不会触发这些自定义操作。

### 手柄操作

需先在**设置 → Xbox / PlayStation 手柄**里开启（默认关闭），选择设备和“自动 / Xbox / PlayStation”提示布局。
支持 Xbox，以及原生 DualShock 4、DualSense 的 USB / 蓝牙基础输入；原生 PS 尚未完成实体手柄实测，欢迎反馈设备名称与连接方式。
Steam Input / DS4Windows 转为 Xbox 输入时，可手动选择 PlayStation 提示。工具**不拦截**游戏的手柄输入，因此同键的游戏原生动作可能同时触发。

按键按物理位置对应：A / B / X / Y = × / ○ / □ / △，LB / RB = L1 / R1，LT / RT = L2 / R2，LS / RS = L3 / R3，Start / Back = Options / Share（DualSense 为 Create）。

| 场景 | Xbox | PlayStation | 作用 |
| :--- | :--- | :--- | :--- |
| 游戏大地图 | LB | L1 | 打开地图工具台 |
| 游戏大地图 | RB | R1 | 打开独立点位助手 |
| 探索中 | 按住 LB 再按 B | 按住 L1 再按 ○ | 完成附近点位；全部松开后执行一次，多点先选择 |
| 探索中 | 按住 LB 再按 X | 按住 L1 再按 □ | 打开附近点位攻略 |
| 任意 | LB + Start | L1 + Options | 开始 / 停止探索 |
| 菜单 / 列表 | 左摇杆 / 方向键、A、B | 左摇杆 / 方向键、×、○ | 选择、确认、逐级返回 |
| 点位助手详情 | 长按 A 0.6 秒 | 长按 × 0.6 秒 | 完成所选点位；X / □ 展开图片 |
| 可批量收集的候选列表 | 长按 X 0.6 秒 | 长按 □ 0.6 秒 | 一键完成整组 |
| 独立攻略窗口 | LS | L3 | 切换游戏与攻略焦点 |
| 独立攻略窗口 | B | ○ | 返回游戏焦点，攻略保持显示 |
| 独立攻略窗口 | LB + X | L1 + □ | 关闭攻略 |
| 独立攻略详情 | 长按 A 0.6 秒 | 长按 × 0.6 秒 | 完成当前点位 |
| 当前路线目标攻略 | 长按 Y 0.6 秒 | 长按 △ 0.6 秒 | 跳过当前路线目标，不修改点位完成记录 |
| 攻略图 | LB / RB、X、右摇杆 | L1 / R1、□、右摇杆 | 上一张 / 下一张、展开图片、滚动 / 平移 |
| 展开的攻略图 | LT / RT | L2 / R2 | 缩小 / 放大 |

独立攻略打开时游戏仍保持焦点，按 LS / L3 后才操作攻略；点位助手内的详情直接获得焦点。一次只处理选中的设备；断线后不会自动接管另一只手柄。
重连、切换设备或返回焦点时，先松开按键并让摇杆回中位。USB 与蓝牙可能显示为不同设备，换连接方式后请重新选择。

### 库街区进度同步扩展

工具可以和[库街区鸣潮大地图](https://www.kurobbs.com/mc/map/)的账号进度**双向同步**：首次合并取两边完成点的并集，
本地独有且云端从未完成的点会保留并写回库街区。后续若已记录的云端完成点被用户取消，本地也会反映该取消；
开启自动同步后，用户在本地标记完成或取消的状态也会写回自己的库街区账号。

IMAO 不修改或伪造库街区公开地图、点位等公共内容。账号进度同步仅在用户主动登录并连接自己的库街区账号后，
使用该用户自己的登录会话，同步该用户自己的点位完成状态。

**Edge 加载项商店（推荐）：[IMao 库街区进度同步](https://microsoftedge.microsoft.com/addons/detail/ohmikfaeobbffhlhoocklplniobcfdbg)**

用 Chrome 的话请按[手动安装说明](Docs/KuroMapSyncInstall.md)加载同一份代码 —— `manifest.json` 里固定了发布公钥，
所以手动加载与商店版的扩展 ID 完全相同（`ohmikf…`），桌面端的凭据与同步档案不受影响。

装好之后：

1. 在工具的**设置 → 库街区点位进度同步**里点「注册/修复浏览器桥接」。
2. 打开并登录库街区大地图，点扩展图标 → 「连接桌面端」。
3. 回到工具，先「预览同步」看差异，再「应用同步」。打开「自动同步」后，本地点位完成或取消状态会立即推送，
   并每 10 分钟整体补齐一次。

凭据只通过浏览器官方的 Native Messaging 通道交给本机程序，由桌面程序以当前 Windows 用户的 **DPAPI 加密**保存在本机。
扩展**不读 Cookie、不访问其他站点、不做任何网络请求**，只读取库街区地图页面自身存放的登录会话。

### 支持的地区

共 **14 个地区、52 个子区域**。地区可在设置页按国家分组查看，
支持停用、删除与重新下载（**改动需完全退出并重新打开软件后生效**）。

**梦枢天罗已开放并可用**（瑝珑，Kuro state 912，独立小世界）。
点位数据、地区元数据、图标、定位特征、校准数据及上游影像覆盖与哈希记录已入库；地区定位资源已按实测上游影像覆盖范围生成，
并**通过实机小地图参考图验证**（误差 2.7 像素），原始地图影像未入库。
四点校准已由 2026-09-30 的四组实机截图拟合（最大误差 0.53 像素），已通过本机实测准入，支持筛选、标注与视觉定位；
完整的四项检查与基线回归仍在进行中。校准流程见
[新地区四点校准样本](Docs/KuroSceneCalibrationSamples.md)。

| 国家 | 地区 |
| :--- | :--- |
| 瑝珑 | 今州、梦州、梦枢天罗 |
| 黑海岸 | 黑海岸群岛、泰缇斯之底、时隙废都 |
| 黎那汐塔 | 拉古那、七丘、下层金库、阿维纽林、隐海试验场 |
| 罗伊冰原 | 冰原地表、拉海洛、黯原 |

当前地区定位资源包的磁盘占用（含楼层特征）从约 3.3 MiB（时隙废都）到约 270 MiB（拉海洛）不等；
这些是最终视觉定位资源的大小，并非上游影像大小或压缩下载体积。随程序分发的地区无需另行下载。
梦枢天罗已确认上游地图影像覆盖 12 个瓦片位置，并据此生成和验证定位特征；这些原始 PNG 不随程序分发。

### 常见问题与已知限制

<details>
<summary><b>点开看详细说明</b></summary>

<br>

- **为什么冷启动要先开一次大地图？**
  区域身份只能由视觉匹配或大地图首次解算确定，游戏坐标读数不能单独决定「我在哪个区域」。
  这一步只需一次，之后进出地区都会自动识别。
- **定位丢失时标记为什么会消失？**
  任一定位数据不可用、结果有歧义或证据不足时，程序会保持上一可信位置最多 3 秒；期间按轨迹外推继续移动标记，
  超过 2 秒没有新结果就不再绘制，避免标记停在原地闪烁。
- **有些地方小地图定位不出来。**
  深色细等高线画风、大片水面与稀疏礁石区域（例如乘霄山、泰缇斯部分区域）特征太少，匹配会被保守拒绝。
  这类位置**打开一次大地图即可定位**——游戏大地图画面与用于构建定位特征的地图影像相匹配，识别非常稳定。
- **分辨率必须是 16:9 吗？**
  不必。游戏把自己的 HUD 按 `min(宽/1600, 高/900)` 这一个比例缩放、每个控件再贴边（上/下/左/右）锚定，
  工具现在按同一套规则定位，所以 2560×1600、3840×2160、21:9 都能用，坐标识别也不再限定那三档 16:9。
  （2026-09-26 之前：客户区不是 1600×900 / 1920×1080 / 2560×1440 时坐标识别会被安全拒绝，
  16:10 下小地图裁剪还会整体偏下、任务图标区偏 36px。）
- **少数楼层的画面高度相似时会怎样？**
  程序会把它们一并当作当前位置显示，而不是猜一个再标成楼上或楼下。
- **新区什么时候能用？**
  新地区需要完成四点校准 + 独立视觉定位特征包 + 实机验证才会开放。下层金库、黯原、时隙废都已通过本机录制试校准
  并可用，完整的四项检查与 113 张基线回归仍在进行中。梦枢天罗（2026-09-30 归档）的地区特征包与四点校准
  都已就绪，并已通过本机实测准入，可用；完整的四项检查与基线回归仍在进行中。
- **会掉帧吗？**
  覆盖层默认只绘制小地图与状态条所需的区域（2560×1440 下小地图窗口约占 **3.3%** 屏幕面积），
  画面读取也只截取探索所需的若干区域，实测读取耗时由约 13 ms 降至约 6 ms。
  若显示或帧率异常，可在设置页切换「叠加层呈现方式」或调整刷新间隔。
- **会不会影响账号安全？**
  IMAO 的定位与覆盖功能基于**游戏可见画面捕获、图像特征匹配和坐标识别**，不读取或修改游戏进程内存、
  不进行代码注入，也不会向游戏发送自动化操作输入；路线规划与完成标记不执行自动寻路、自动战斗或自动收集等游戏操作。
  它不会、也无法保证第三方反作弊的判断，请自行评估后再使用。

</details>

### 更新失败：「更新地址必须来自本项目的 GitHub Releases」

**2026-09-24 之前构建的所有版本（含 2026.9.21.1、2026.9.23.1）都会卡在这里。**
项目仓库改过名，而这些版本的更新器把旧仓库地址写死在校验里；GitHub 会把旧地址
`301` 重定向到新地址，旧更新器拒收重定向目标，于是**下载任何更新都会失败**（检查更新能显示新版本说明，
但一点下载就报错）。这是程序自身的缺陷，与你的网络无关。

**处理办法（一次性，按顺序做）：**

1. 到 **[Releases](https://github.com/kahvia-d/IMAO/releases/latest)** 手动下载 **`IMao-v<版本号>-windows-x64.zip`**。
2. **完全退出**正在运行的 IMao（托盘图标也要退出）。
3. 把压缩包解压到**一个新目录**，运行其中的 `IMao-Launcher.exe` 确认能正常启动。
4. 确认无误后，再用新目录替换旧的安装目录（或直接改用新目录，删掉旧的）。

> **你的数据不会丢**：点位完成记录、路线、筛选与个人设置都存在
> `%LOCALAPPDATA%\IMao-WinUI` 下，**不在程序目录里**，替换程序文件不影响它们。
> 换好之后自动更新即恢复正常，以后不用再手动换包。

## 开发

想自己编译或参与开发，请看：

- [可复现构建（Windows x64）](Docs/Build_zh-Hans.md)
- [CMake 编译说明](Docs/Compile_zh-Hans.md) / [Compile with CMake](Docs/Compile_en.md)
- [文档索引](Docs/README.md)（构建、更新、地图数据、界面、审计记录分类归档）
- 项目结构：`IMao-Core/`（C++ 原生核心：视觉定位、资源加载、IPC）、
  `IMao-WinUI/` + `IMao-WinUI.Core/`（WinUI 3 外壳：界面、更新、设置）、
  `tools/UpdatePublisher/` + `scripts/`（签名清单与发布链路）、`BrowserExtensions/KuroMapSync/`（库街区同步扩展）。

本项目基于 [IMao-Wuthering-Waves](https://github.com/Yepin2022/IMao-Wuthering-Waves) 持续修复与二次开发。

## 致谢

### 开源库

- 图像识别：[OpenCV](https://github.com/opencv/opencv)（含 opencv_contrib，使用 SURF）
- 文字识别：[PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) / [Paddle Inference](https://www.paddlepaddle.org.cn/inference/master/guides/install/download_lib.html)
- 窗口捕获：[Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample)
- 覆盖层界面：[ImGui](https://github.com/ocornut/imgui)
- C++ JSON：[nlohmann/json](https://github.com/nlohmann/json)
- WinUI 组件：[CommunityToolkit/Windows](https://github.com/CommunityToolkit/Windows)、
  [microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml)、[WinUIEx](https://github.com/dongle-the-gadget/WinUIEx)

### 数据与定位资源来源

- **点位、分类及相关地图信息**：来源于[库街区《鸣潮》大地图](https://www.kurobbs.com/mc/map/)公开可访问的数据。
- **视觉定位特征**：构建工具读取库街区大地图公开可访问的 1024×1024 地图影像，灰度化并提取 SURF 关键点与特征描述子，转换到 IMAO 的地图/世界坐标后生成定位资源。
- **原始地图影像**：仅作为构建阶段的输入，归档在 `map-regions/tiles/...` 等本地目录；原始瓦片与分层影像由 `.gitignore` 排除，只提交 manifest、哈希等记录。**原始地图瓦片不包含在本项目 Git 仓库或发行包中。**
- **实机验证参考图**：部分地区特征包包含维护者从游戏可见画面采集的 `reference-minimap.png` 小地图参考截图，仅用于定位校准与验证，不是库街区原始地图瓦片。

构建流程：`公开可访问的地图影像 → 灰度化 → SURF 特征提取 → 坐标转换 → IMF 特征数据 → IMX 视觉索引`。
发行包中的地区定位资源（内部目录名为 `KuroTilePacks`）主要是关键点、特征描述子、空间坐标、
`.imf` 特征数据、`.imx` 视觉索引及 manifest 等机器视觉定位数据，**并非库街区原始地图影像**。

地图数据与视觉定位构建流程使用库街区大地图正常向普通网页用户公开提供的数据和地图资源，
不依赖破解、解密或绕过身份验证、付费墙或技术访问控制来获取这些公开资源；公开可访问并不自动意味着取得使用授权。

感谢所有参与开发、测试与反馈的朋友们，是大家的帮助让这个工具越来越好！(\*´▽｀)ノノ

## Star History

<a href="https://www.star-history.com/?repos=kahvia-d%2FIMAO&amp;type=date&amp;legend=top-left">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=kahvia-d/IMAO&amp;type=date&amp;theme=dark&amp;legend=top-left" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=kahvia-d/IMAO&amp;type=date&amp;legend=top-left" />
    <img alt="IMAO Star 趋势图" src="https://api.star-history.com/chart?repos=kahvia-d/IMAO&amp;type=date&amp;legend=top-left" />
  </picture>
</a>

## 加入我们

**用户交流 QQ 群：`1109700733`**

遇到 Bug、想要新功能，或者只是想找个地方聊鸣潮，都欢迎进群；也欢迎到
[Issues](https://github.com/kahvia-d/IMAO/issues) 反馈。

如果这个工具帮到了你，**帮忙点个 Star 吧**！~（网页右上角的小星星）这就是对我们最大的支持了！

## 声明

- IMAO 是免费、开源的非官方《鸣潮》社区地图辅助工具，软件代码按照 [GNU General Public License v3.0](LICENSE) 提供，不附加用途限制。
- GPL-3.0 适用于本项目有权许可的软件代码及项目自有内容。涉及《鸣潮》、库街区及其他第三方的名称、商标、图片、图标、攻略和其他素材，其相关权利仍归各自权利人或内容作者所有。
- IMAO 与库洛游戏（Kuro Games）及库街区不存在官方隶属、授权、赞助或背书关系。《鸣潮》、库街区及相关名称、商标、地图、图像、图标、游戏素材和其他第三方内容的相关权利归其各自权利人所有。
- 点位、分类及相关地图信息来自[库街区《鸣潮》大地图](https://www.kurobbs.com/mc/map/)公开可访问的数据；视觉定位资源由构建工具使用公开可访问的地图影像提取 SURF 特征、转换坐标并生成视觉索引。
- **原始库街区地图瓦片不包含在 Git 仓库或发行包中**。发行的地区定位资源是机器视觉特征与索引数据，并非原始地图影像；少量实机小地图参考截图仅用于定位校准与验证。
- IMAO 不修改或伪造库街区公开地图、点位等公共内容；账号进度同步仅在用户主动登录并连接自己的库街区账号后，使用自己的登录会话同步自己的点位完成状态。
- 游戏定位与覆盖功能基于可见画面捕获、图像特征匹配和坐标识别，不读取或修改游戏进程内存、不进行代码注入、不向游戏发送自动化操作输入，也不执行自动寻路、自动战斗或自动收集等游戏操作。
- IMAO 本身永久免费，没有付费版、会员版或功能解锁。GitHub Releases 与 QQ 群文件均提供免费获取方式；Mirror酱仅为需要 Mirror酱 CDK 的可选第三方高速下载与更新服务，不会解锁额外 IMAO 功能，不使用它也不影响正常功能。
- 本项目无法保证第三方平台或反作弊系统的判断，请用户自行评估后使用。如果相关权利人或内容作者认为特定内容存在来源、署名或使用方面的问题，欢迎通过 [GitHub Issues](https://github.com/kahvia-d/IMAO/issues) 联系维护者，我们会核实具体内容，并视情况更正、补充署名、调整或移除。
