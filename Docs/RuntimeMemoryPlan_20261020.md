# Runtime Memory Implementation Plan

## 当前优先级更新（2026-10-21 22:00后）

用户明确决定：单引用旧截图最长7.130秒的问题先记录，暂不继续排查或修复。停止时会释放，当前不定为泄漏。此前“下一步优先归属跨秒旧图”已被本次指示暂缓，保留历史证据。继续调查剩余约0.9GiB进程内存中的大项，区分全地区常驻资源、活动定位缓存/工作区、字体/图形、OCR与未归因私有内存，寻找较大且可验证的优化。楼层样本编码及停止态缓存释放仍沿用暂缓决定，Direct显示和全部定位规则不变。


## 当前优先级（2026-10-21 字体验收后）

用户本轮决定：楼层float样本编码存储先登记，暂不实施；继续拆解图形、采集和OCR运行增量，优先寻找更大的占用来源。后续验证该样本优化时，保留全部样本、顺序、stride/ownArt及最终票数判定，理论18.41MiB不作为已实现收益。停止态缓存释放也暂列后续，不用它冒充活动探索态改善。

用户要求优先继续内存归因。13:55:34 的输入区段间隔 275ms 暂登记待查：boundsMs=275、trackMs=16、presentMs=10、mapBuildMaxMs=3.9131；字体预热在约9秒前完成且当时尚未安装输入钩子。该指标不是硬件鼠标延迟，也尚未定位到具体Win32调用。本轮不展开停顿修复。

字体现场日志已确认 rgbaRetainedBytes=0；探索40/45/50/55/60/90秒工作集为934/921/925/1171/985/959MiB，OS峰值1184MiB。路线不同，不能把与上轮的差值全部记为字体收益。接下来用独立进程逐阶段记录真实Mat所有者及进程工作集/private，分开常驻资源、候选缓存、临时峰值和释放后的残留；不以总进程前后差值冒充模块独占字节。

### 本轮账本结果

新增离线诊断 `IMaoVisualRegression --memory-ledger <root> <manifest> <output-prefix>`，使用实际快照、楼层加载器及两个定位器，按UMatData记录CPU Mat真实分配/销毁（浅拷贝与ROI不重复计数），不改生产分配或工作集策略。实际所有权自检、8项既有viewport回放和2个受影响原生套件通过。

全14地区：地图编码描述子143.56MiB，关键点容量31.40MiB，视觉索引数组84.21MiB，词典2MiB；楼层全部Mat59.98MiB，其中编码35.44MiB、float样本24.54MiB。索引各vector的capacity=size，无大块闲置容量。生产层索引与资源预载并行，旧日志两个阶段进程差不能当楼层独占字节。

当前App::StopTasks只取消两个定位器的任务；定位器及OCR在核心Shutdown才销毁。探针保留两定位器后取消，Mat仍267.79MiB；销毁两个定位器后205.61MiB，回到资源Mat基线。真实Mat所有者减少62.18MiB，private约降89.94MiB；差额含索引、STL和分配器状态，不能全部称描述子。全部资源/楼层释放后Mat=0，private22.14MiB，不将其与初始差额直接称为堆保留。范围不含OCR、采集、图形、dense tile确认，不能据离线结果排除整个程序的其它遗留。

停止后测试CoreHost的只读地址/驻留检查：WS约693.08MiB、private664.35MiB；私有区域驻留625.34MiB、映像驻留65.39MiB、文件映射驻留2.32MiB。大部分属于私有区域，仍不能进一步区分对象、空闲堆块或驱动私有分配。检查未注入、挂起、读取进程内存内容或裁剪工作集。

下一候选是楼层float样本保留同源编码行、匹配时逐楼层展开，理论常驻差约18.41MiB；必须保留所有采样行、stride/ownArt对应和完整最终投票，先做描述子及票数对照。停止态缓存释放策略和图形/采集/模型仍需继续单列。证据：`out/cpu-memory-analysis/memory-ledger-20261021/`。本轮仅诊断工具与账本，无生产策略调整。

### 下一轮现场采样：完整彩图唯一分配

**现场结果（17:54:59–17:57:44，约2分45秒）：** 81条运行采样中大彩图存活中位95.90MiB、p95 113.02MiB、采样最大124.81MiB；逐次allocator历史瞬时峰值143.23MiB。WGC内部两份28.91MiB之外，交付帧同时2–5份（通常3份43.36MiB，最大72.27MiB），motion为2048×1008 BGRA同时0–4份（最大31.5MiB），viewport请求与候选anchor各最多一份7.875MiB。各来源最大值不能直接相加，标签也不等于reader归因。

停止后frame-storage liveBytes=0、allocations=0、trackingFailures=0，正常关闭callbacksDrained=1 closeErrors=0，无新dump；只证明本轮受跟踪大彩图释放，不等于整个进程回到基线。进程定时工作集约874–983MiB，OS峰值1213MiB。仍有一次288ms输入间隔（boundsMs=288），并入原275ms待查项，继续内存优先。

本轮没有内存优化实施，实测的约96MiB常态/143MiB峰值不是全部可删除。下一候选是motion保存数据的大小，以及完整帧reader保留生命周期；ScreenMotionTracker先缩放彩色再转灰度、小地图另用颜色生成mask，不可直接先转灰度视为等价。证据`out/cpu-memory-analysis/frame-storage-field-test-20261021/`。

已加入默认关闭的`IMAO_FRAME_MEMORY_TRACE=1`诊断。安装于资源/采集工作线程启动之前，只记录>=1MiB CPU BGR/BGRA图像的真实UMatData，最终deallocate扣除；ROI和浅拷贝不重复计费。按WGC内部、交付帧、motion、viewport请求/候选见证和缩放来源分组；标签表示分配点，不冒充具体reader。每2秒与会话完全退出后记录liveBytes、进程内累计图像peakBytes、数量、尺寸和trackingFailures。灰度/float/小图/GPU/堆页不在这个账本内。

真实SnapshotChannel、旧reader保留、跨线程最终释放、ROI/clone及并发BGR分配测试先RED后GREEN；独立审阅发现诊断异常隔离问题，修复为不影响工作线程。元数据失败注入先RED，修复后保留原成功分配图像及base allocator，显式标记dataIncomplete。Report/Install异常也显式报错；复核无阻断点。CoreHost及相关原生目标Release构建、13/13受影响回归通过（42.26s）。

已脚本刷新并重启测试树，398项载荷differing=0、自包含和resources.pri校验通过，刷新时4份地区激活/选择文件哈希不变；重启后测试版activation按预期切换至新bundled快照，16个包的完整定义逐项不变，地区selection及玩家版状态文件不变。启动日志17:45:01确认frame-storage-enabled；实际同时持有份数仍待现场探索采样，尚不宣称有内存优化收益。证据`out/cpu-memory-analysis/frame-storage-20261021/`及`frame-storage-deploy-20261021/`。

### 继续拆解：图形、灰度瓦片与OCR

按用户决定，楼层样本编码方案暂缓，继续独立组件实验。实际产品ImGui/DX11 backend、实际字集、隐藏2560×1440窗口与12帧文字draw：硬件设备初始化相对WinRT准备后WS约增24.91MiB、private约增46.56MiB；设备/交换链/字体/draw总增量约106.77MiB WS、202.86MiB private，包含CPU字体和GPU/驱动状态，不能再与字体逻辑大小相加。GPU字体仍为4096² RGBA8逻辑64MiB，CPU RGBA已释放；R8单通道方向理论纹理差48MiB，但尚未实施，也不能据此承诺任务管理器下降48MiB。

第二采集设备、相同swapchain/frame-pool/compositor构建及full staging分别测量。full staging逻辑14.06MiB，Map后private约增14.09MiB；合成现场CPU缓冲字节30,313,584另增28.91MiB。这不是实际WGC session/callback复现，未启动会话的pool读数不能代表运行池。采集内部两帧日志未计App发布帧、旧帧读者、查询裁剪和motion image。后续继续测这些唯一分配的生命周期。

图形显式对象释放、ImGui live归零后，硬件探针仍约46MiB WS/138MiB private；不能直接称泄漏或全部称空闲堆。显式dispatcher Shutdown与仅释放controller的独立对照均约138MiB private，不支持本次残留主要来自dispatcher未关闭。WARP对照约98/98MiB，不作为替代硬件呈现的优化建议。

新增`IMaoVisualRegression --dense-memory <MapTiles-root> <ledger.jsonl>`，真实DenseMapConfirmer、测试树64张不同瓦片×3轮：存活Mat在48张时达到48.03MiB，继续访问不再增长；释放输入后精确48MiB。证明有界静态灰度缓存，不代表此前现场路线已填满上限。常量输入只测缓存，均未接受，不当准确率用例。

OCR当前默认配置复测36次、1122条检测与此前默认输出逐项相同，三轮均约58.20MiB WS/48.27MiB private，销毁后14.48/7.23MiB；未观察到本次持续增长。生产识别器源SHA与既有provenance一致，未改变arena/pattern。

硬件shutdown、硬件release-controller、WARP shutdown三个图形探针exit0且ImGui最终live=0；原生诊断工具Release构建及MinimapFilter/MultiSceneViewport 2/2回归通过。证据和完整归因限制位于`out/cpu-memory-analysis/graphics-ledger-20261021/`。本轮继续只做诊断，无产品内存策略改变。

> 执行方式：沿用当前分支和 inline executing-plans，按用户已批准的“归因→最大项优化→同场景验收”继续。

**Goal:** 找出资源初始化后新增内存，先消除可证明的匹配数据重复持有，保持特征、搜索参数、接受规则和地区选择不变。

**Architecture:** 独立进程探针测 SURF 与 FLANN 的工作集/private/峰值，显式统计可计算矩阵与索引字节。单个候选矩阵生命周期内直接构建同参数 KD-tree，持有 Mat 引用直到索引销毁，避免 matcher 合并副本与 miniflann clone。缓存按真实描述子、索引及关键点持有字节计费；光标线程不参与测量或释放。

**Tech Stack:** C++20/OpenCV 4.11/Win32 process memory；只使用本机代码与截图。

- [x] 探针及回归 RED：旧 FLANN 在一个不可变训练矩阵外还持有两份描述子；验证所有权、同 RNG 同参数的两个最近邻和距离。
- [x] 原生同参数单矩阵 matcher；查询维持 checks32/trees4/L2平方根与原 DMatch 行号。实测而非推测索引常驻字节。
- [x] 请求级 SURF/查询缓存、匹配索引计费诊断；探针分别测 SURF、训练及释放，避免并行阶段差值误称模块独占。
- [x] 真实资源、实际截图回放与原 matcher 同输入对照；确认缓存预算不会让全局 sweep 丢覆盖或反复重建。
- [x] CoreHost Release 与受影响回归、独立审阅；记录实际收益和仍未归因部分，不承诺600MB。

约束：不改变任何既有断言、匹配阈值、地区启用状态、楼层证据或 Direct 显示；不修改玩家安装。独立探针不读取或修改运行中的客户端。


## 实施与验证结果

### 可归因的内存

本机 OpenCV 4.11 源码显示，`FlannBasedMatcher::train` 先复制到 DescriptorCollection.mergedDescriptors，再由 miniflann Index::build 复制到 features_clone。原缓存只统计第一份描述子和 keypoints，未计这两份副本和 KD-tree。

独立新进程、80,000×128 float 同一输入：

| 阶段 | 原 matcher 工作集 | 单矩阵 matcher 工作集 |
| --- | ---: | ---: |
| 输入候选就绪 | 45.8 MiB | 45.4 MiB |
| 训练完成 | 144.4 MiB | 65.8 MiB |
| matcher 释放后 | 46.5 MiB | 45.9 MiB |

移除的描述子副本严格为 81,920,000 字节（78.125 MiB）；进程训练工作集减少约78.7 MiB，包含不同进程的小幅基线差异。新索引 reported usedMemory 为20,860,088字节，描述子仍为40,960,000字节。

整张2560×1440存档截图以SURF(100,4,3,true,true)在1/1.5/2/.75缩放下提取，2倍时进程峰值约893.4 MiB，销毁临时数据及SURF对象后约36.8 MiB。该探针使用整张截图，实际产品使用裁剪区域，因此不能把893.4 MiB直接当成产品某次调用的独占内存。探针表明这是需要继续处理的瞬时峰值，不能把所有增长都归为缓存或永久泄漏。本轮没有改变SURF参数或缩放覆盖。

九个场景按现有4MiB/64tile规则选取代表性窗口，含实际索引及关键点的总计费40,850,728字节（39.0 MiB），仍可容纳在默认48MiB预算内。这是按索引顺序取窗口的代表性探针，不声称所有实时排名组合完全同样大小；实际miss日志逐项报告descriptorBytes/indexBytes/entryBytes/heldBytes。

### 已落地的修复

- `OwnedFlannMatcher` 持有候选Mat引用，在同一数据上构建cvflann::Index<L2<float>>；trees4、checks32、L2平方根转换、DMatch行号保持原实现，描述子生命周期覆盖索引，禁止候选在索引活跃时改写。
- 缓存计入描述子、reported KD-tree pool/vind内存、keypoints容量。超过预算的条目供本次完整场景验证使用，完成/取消后释放，避免把一个大窗口长期留在缓存；不削减匹配候选。
- 增加前16次查询提取的阶段读数、查询矩阵字节和WGC双帧CPU字节/逻辑staging纹理大小；显式纹理大小不等于GPU或驱动驻留。进程级前后读数仍受其它线程影响，不能称独占分配追踪。
- `matcherCacheBytes` 加入结果诊断。实际所有者计数在真实index/Mat释放后减少，用于验证完成和取消路径均释放临时索引。

### 红绿与审阅

- 原FLANN包装：保留恰好一份描述子、单独报告索引字节两组断言RED；单矩阵实现GREEN。同RNG种子下2/128/4096行训练，所有最近邻行号和距离精确一致；调用方释放原Mat后仍可重复查询。
- 100,000行实际worker场景先RED：当前定位正确，但超预算数据仍留在缓存；修复后当前定位仍正确，cacheBytes=0。
- 独立审阅没有确定代码缺陷，指出缓存计数不足以证明临时索引释放。已补实际owner字节归零检查：正常完成和观察到索引实际构造后取消，均在结果发布前释放。临时去掉Worker末尾reset时两条生命周期断言RED；逐字节恢复后GREEN。
- CoreHost、VisualRegression、OwnedFlannTests和探针Release构建通过；受影响8/8原生回归通过（33.36秒），没有修改既有断言。保留原有数值转换和zlib CRT链接告警。
- 旧、新程序实际截图顺序回放均8/8通过：6个正例场景相同，2个负例均拒绝，正例坐标差最大0.101地图单位；未把两次序列输出称逐位相同。噪声负例的best-attempt诊断scene从1变0，均未接受或发布位置。单独在新进程回放0.5缩放正例，场景、坐标、203个matches、198个inliers和接受plan完全一致；全流程有预算进度、缓存和随机状态，不据一次计时声称CPU提升。

## 限制与后续

本轮证明了匹配器副本与超预算保留的问题并修复，尚未把整个约500MiB运行增量逐项全部归因，也未解决SURF大图瞬时峰值或保证实机低于600MB。缓存正确计费可能减少能缓存的大窗口，因此实机需同时观察重建频率、CPU及定位延迟。完整证据位于 `out/cpu-memory-analysis/post-deploy-20261020/`，含flann-before/after.csv、surf-before.csv、sweep-storage.csv、红绿日志、截图前后报告和pair comparison。

## 实机复测后的归因与 OCR 入队复制修复

20:18:26–20:22:38 实机日志：14个区域、98个楼层，资源保留486MiB；探索首帧646MiB，OCR就绪873MiB，开大地图前887MiB，大地图操作采样952–1038MiB，关闭3秒后1001MiB，系统记录峰值1143MiB。匹配缓存最高50,167,680字节（47.84MiB），在48MiB预算内。129次结果均为local-512（127次接受），不能据此验收全局搜索/超预算释放。输入处理间隔最高32ms，没有多秒阻塞记录；该指标不等于端到端硬件光标延迟。

### 独立进程实验

为避免把并发增长误算给OCR，使用当前原生识别器源码的临时诊断副本，在相同模型、运行时DLL、4/1线程、图优化、输入预处理和解码下，分开测DLL、环境、会话、预热、3轮识别及释放。12张本机已存档raw/contrast输入，共36次Detect，各变体1122条检测逐项完全一致。这是既有输入上的一致性比较，不是新增准确率数据。

| 设置 | 三轮后工作集 | private committed | Detect中位耗时 |
| --- | ---: | ---: | ---: |
| 默认 | 58.75MiB | 48.38MiB | 5.16ms |
| 关闭CPU arena | 47.65MiB | 31.34MiB | 6.03ms |
| 关闭memory pattern | 53.16MiB | 39.97MiB | 5.11ms |
| 两者均关闭 | 46.49MiB | 29.87MiB | 7.36ms |

默认预热完成53.16MiB，识别器释放后14.52MiB。关闭内存池的收益小，且单次实验显示耗时上升，因此没有改变生产会话设置。227MiB并发区间增量不能全部归因于YOLO模型；该探针不包含Paddle静态依赖、采集、图形与其它工作线程。

新增 `IMaoVisualRegression --minimap-memory <snapshot.json> <minimap.png> prepare|global`。同14包快照、实际运行时加载器、独立进程：资源就绪约272MiB，初始化定位器约275MiB，3次小地图提取约279MiB；全局恢复搜索后约342–347MiB，关闭定位器约279MiB，资源释放后约17MiB。搜索带来的约64–70MiB保留符合候选行缓存；这些读数不含楼层索引或图形组件，不能直接当整个程序基线。

另加 `--subset-memory <features.imf>`，读取编码源并对全包行去重、展开3轮，报告工作集/private/峰值、输出行数及描述子与位置顺序哈希。梦枢天罗/罗伊地表/瑝珑分别观察到约36.6/88.0/168.0MiB峰值，释放候选后约16.3/26.2/43.6MiB；源释放后约10MiB。本轮未改去重规则，也未把这些临时分配称为泄漏。

### 已落地的独立问题

`CoordinateRecognizerRuntime::Submit` 原先每次clone整张截图，YOLO工作线程只使用坐标条。本次仅调整YOLO异步队列：入队时复制所需ROI，私有队列同时保存原始画面尺寸。识别依然根据原始尺寸决定HUD校准，使用相同像素、模型与解码；请求代次、frame/request/session、时间戳及完成通知不变。旧Paddle与同步诊断路径保持原语义。前16次入队增加 `ocr-input-storage`，分别报告源画面与实际拥有的字节。

2560×1440 BGRA每请求像素从14,745,600字节降至80,256字节（减少约99.46%，每份约13.99MiB）。同时有执行中与待处理请求时可避免多份整帧副本，但队列占用与分配器回收影响实际工作集，不能据此承诺总内存固定下降28MiB，更不能声称达到600MB。

实际异步运行时测试通过自定义MatAllocator只观察提交线程分配：旧实现RED（11,059,200字节 > 60,192字节），修复后GREEN（60,192字节）。保留原有缓冲复用、实际识别、元数据和失败初始化断言；新增1920×1080、2560×1440、2560×1600、3840×2160的BGR/BGRA非连续图像，以及不支持尺寸/空输入共12例，逐项对照同步路径的状态、候选、分数、文本和标识，并在提交后立即清空原帧。未修改任何既有断言。

CoreHost/VisualRegression/CoordinateRuntimeTests Release构建通过；异步运行时验证及12个扩展用例通过，Optimization/MultiSceneViewport/ViewportBudget/OwnedFlann与坐标解码/连续性/读数运动/调度共8个受影响原生套件通过。首次CTest的后4项因尚未构建而Not Run，构建对应目标后全部通过；没有放宽断言。独立审阅未发现确定缺陷。证据在 `out/cpu-memory-analysis/ocr-isolation-20261020/`，含探针源码副本与源SHA、配置/检测比较、分配红绿日志及扩展运行时验证。测试树尚未部署本次ROI修复。下一轮继续区分采集/渲染资源和其余候选状态，实机确认总工作集与复制量。

## 全局查询峰值与过期任务（OCR 修复验收之后）

OCR版已在20:46刷新测试树。20:46:34–20:49:07复测16条诊断均为源14,745,600字节、实际拥有80,256字节；常规大地图前30秒定时工作集中位数952MiB，上轮993.5MiB。操作不同，不把全部差值归因于ROI。此次另触发4次全局搜索；20:48:14有2.29秒任务，1.5/2倍查询期间系统峰值1242MiB，后到1244MiB。全局查询的临时分配与过期结果是本节目标，保持全部尺度、特征、场景歧义和最终接受规则。

### 根因与同输入实验

本机OpenCV-contrib4.11.0的 `fastHessianDetector` 同时分配所有octave的determinant/trace矩阵。参数4octaves、3layers实际生成20组矩阵；2倍输入有4倍像素。查询输出描述子远小于这个检测工作区。源 `surf.cpp` SHA256为 `6b0987145a065a282fab0642647e973787dc385e5894ede91fe287c33a0648ff`。

独立进程、同一存档2560×1440大地图、相同产品地图裁剪和中心裁剪，分别测1/1.5/2/.75八组：

| 项目 | 原全金字塔 | 三层复用 |
| --- | ---: | ---: |
| 4096×2016灰度、2倍完整裁剪，额外Mat峰值 | 471,736,196字节（449.88MiB） | 231,235,460字节（220.52MiB） |
| 八组独立进程总工作集峰值 | 507,146,240字节（483.65MiB） | 272,101,376字节（259.50MiB） |
| 2倍完整裁剪特征点 | 24,117 | 24,117 |

八组特征点全部字段及顺序、描述子每个字节相同。进程峰值约减224MiB；这不是整个客户端的实机降幅。两个实现同进程的compare模式只用于字节对照，其OS历史峰值含两种实现，不能当新实现单独峰值。单次提取时间有下降，尚不据此宣称持续CPU百分比改善。

### 实施

- `StreamingSurf` 只改Hessian工作区生命周期和独立行并行：每octave保留相邻3层；过滤、3×3×3极值、插值、排序不变；全部4octave/3layers保留，描述子仍调用原OpenCV SURF compute。灰度输入与100/4/3/extended/upright参数不变。在层间检查取消，取消时不暴露部分输出。
- 查询仍先缩放彩色像素再转灰度；完成转换即释放缩放彩色副本。请求内查询缓存与每个完整歧义分组仍保留原语义。全局提取逐项记录resize/gray/detect/describe耗时、灰度/描述子字节和detectorScratchBytes，最后一项只算显式积分图与Hessian矩阵，不包含所有分配器/库开销。
- App提交时只clone一次地图裁剪，工作线程与后续图像桥接共享该冻结Mat。旧路径的工作线程持有整帧ROI视图、桥接另存裁剪；新路径不通过该请求额外保留整张截图。工作线程与桥接均只读。
- 仅全局任务运行至少250ms后，每250ms检查当前图像，连续2次无法保留源帧时取消epoch并通过原有静止/拖动延后门重新调度；Local512不取消。近乎不变的归一化灰度（平均绝对差≤1）直接保留任务，避免静止稀疏地图因缺少运动拟合支持而饥饿。此见证只决定是否继续工作，最终位置仍必须通过原有匹配、歧义、时效与几何桥接检查。
- 上游版权保留在源文件及 `Assets/Licenses/ViewportSURF.txt`；原生构建另生成根级 `ViewportSURF-LICENSE.txt`，供整树更新脚本同步，发布包由StageAssets包含notice。

### 红绿与边界

- 实际MatAllocator跟踪分配/销毁：原实现超过工作区上限而RED；三层版GREEN。永久测试的 `--reference-as-candidate` 对照同样RED，没有改既有断言。
- 12组随机、空白、重复图案/非连续输入（含奇数、小尺寸）逐项对照原SURF；5个检测/描述子阶段取消点均无部分特征，实际Mat持有归零。
- 审阅发现初版过期见证会把静止稀疏图误判为过期。补全平面、40×40集中纹理、整体+1灰度三例，先RED；改为保守保留见证后GREEN。纹理充分的静止/平移、无关画面、空图/尺寸变化和调度间隔、两次失败、新请求重置测试一并保留。审阅复核没有剩余确定缺陷。
- 特征相同不代表整个含随机索引/预算的定位流水线逐位相同；回放以原fixture接受/拒绝、场景、坐标容差为准，不放宽任何接受规则。首次并发回放时旧版噪声样例超时，单独重跑旧版通过；并发时延不作性能比较。
- CoreHost、VisualRegression、新增两个测试目标和受影响Optimization目标Release构建通过；最终10/10受影响原生套件通过（43.25秒）。单独运行的新旧回放均8/8通过，六个正例场景一致、坐标差最大0.15544地图单位，远小于原fixture5单位容差；两个负例均拒绝。原生源更改未涉及托管/XAML，未以托管测试替代WinUI构建门禁。

证据目录 `out/cpu-memory-analysis/viewport-surf-20261020/`：探针源码/上游SHA、baseline/streaming/compare.csv、永久内存对照红绿日志、freshness红绿日志、构建日志与新旧定位回放。当前仅修改开发工作树，尚未把本节改动部署到测试树；实机总峰值和拖动恢复时间仍待验收。单个OpenCV描述子计算/索引构建仍非可抢占，2秒仍是完整歧义组之间的软预算，未承诺硬截止或全地区低于600MB。

## 描述子秒级尾延迟：独立有界分块执行（2026-10-20）

最新实机 request206 的4096×2016提取 detect126.7ms、describe1470.3ms。排查发现本机OpenCV4.11的parallel_for_用函数静态原子flagNestedParallelFor；另一线程的短并行区域可能使整批SURF描述子退回单线程，另一任务退出后本批也不会重新并行。定位循环的灰度/缩放/ORB与异步匹配存在重叠调用路径。原日志没有当帧图像、尺度分布、线程并行证据，故不能断言这就是历史1470ms事件的唯一原因。

固定真实截图、4096×2016、24117个关键点、预热后交替4轮：旧版无竞争140–629ms，另一并行区域仅等待50ms时1442–1653ms；显式单线程994–1689ms。竞争线程不做CPU密集计算。16项均逐关键点字段及描述子字节一致，复现了并行度退化，而不是依赖更少特征达到速度变化。

修复把本机OpenCV-contrib4.11.0的SURFInvoker原核逐字符保留，直接调用相同算术，以32关键点为块、最多4个参与线程（包含调用匹配worker）分发。不修改OpenCV全局线程数，不增加输入线程等待或全局锁；保持积分图单份、关键点及描述子行顺序、不可表示尺度的原删除/行压缩规则。回调串行执行，异常或取消停止分发，join所有已启动线程后传播异常，不移交部分结果；线程创建失败也先join。不承诺单个块内硬实时抢占，索引构建仍非可抢占。

新执行方式同图对照16项：无竞争270–279ms，持续冲突261–302ms，50ms冲突258–282ms；所有关键点字段/描述子字节与原核一致。4线程上限意在限制CPU并稳定尾部，不能声称比16线程旧版最快140ms普遍更快。日志增加descriptorWorkers（配置的参与数上限）、descriptorWindowPixels、descriptorMaxWindow，辅助区分未来的窗口工作量变化及系统调度问题。

红绿：新增第40个取消检查点（落在描述子块之间）在旧实现失败两条断言；新版通过。新增竞争区域下实际多线程回调、完整特征一致性、有界工作线程数、窗口诊断，以及回调抛异常后join/释放测试。原12组随机/空白/重复图案/奇数/非连续输入等价断言、实际Mat峰值上限及5个取消点保留未放宽。真实截图8个region/scale提取逐字段及描述子字节全部精确一致。独立审阅确认SURFInvoker与上游完整结构逐字符一致，未发现可操作问题；线程创建失败路径仅静态检查，未注入系统线程资源耗尽。

CoreHost、VisualRegression、StreamingSurfTests、OptimizationTests Release构建通过，10/10相关原生套件通过（34.64秒）。完整旧/新独立定位回放均8/8，6个正例接受且场景一致、坐标最大差0.11672地图单位（原fixture5单位容差未改）；2个负例均拒绝。特征字节一致不等于含随机索引/预算的全流程逐位一致。仅原生源码变化，无托管/XAML修改。

证据：out/cpu-memory-analysis/descriptor-investigation-20261020/（根因探针及限制），out/cpu-memory-analysis/descriptor-fix-20261020/（取消红绿、同图16项执行对照、8组真实特征比较、10项回归、完整定位前后报告）。本节记录的是实验收益；实机收益需部署后复测，不承诺总内存下降或600MB目标。

## 采集关闭生命周期修复（2026-10-21）

实机2026-10-20 21:52:56的CoreHost异常为0xc0000005读null。使用该版本原OBJ/原link命令顺序离线生成MAP，部署版本与离线重链.text的3,348,480字节及RVA完全一致，异常在SimpleCapture::ProcessFrame的Unmap前接口解引用。该版本FrameArrived保存裸this；Close关闭session/pool后即允许析构继续，没有明确排空正在执行的回调，也没有令迟到委托与owner脱离。m_d3dContext正常路径仅在构造时初始化，成员析构会释放它，存在与异常吻合的关闭生命周期危险窗口。不能仅凭该窗口断言历史事件没有其它内存破坏原因。

新增CaptureCallbackGate：WinRT委托仅强持独立gate，owner为gate内受锁保护的非拥有指针。Invoke独立锁覆盖整个帧处理（包含异常报告），StopAndWait先原子关闭入口，再获取锁等待活动回调退出并清空owner。SimpleCapture::Close在析构体内先排空、撤销FrameArrived，再逐步关闭session/pool及释放相关成员；迟到委托只访问仍存活的gate，不能再访问owner。Close串行化且无异常外逃，单步骤关闭失败保留步骤、HRESULT/异常文本，后续清理继续；日志自身失败输出调试诊断。新增capture-wgc-closed记录callbacksDrained和closeErrors，用于实机停止验收。

gate锁与m_frameMutex分离，取最新帧仍只使用后者；新等待仅位于回调互斥及关闭流程，不把GPU读回时间移回日常消费者。当前可达Close调用仅来自析构，帧回调不自关闭；未来若增加回调内Close调用，必须重新设计排空，不能在回调内等待自身。

测试先运行原裸owner/无排空契约的安全模拟负控制，7条断言失败；marker保留分配以避免真的触发C++悬空引用UB。新gate测试通过：Close等待活动回调后才能标记资源释放、正在执行帧完成一次、关闭后拒绝迟到委托、owner销毁后gate仍可安全接收拒绝请求、异常释放锁、重复关闭。握手控制了回调已进入、关闭已发起的顺序，但50ms等待断言仍受线程调度影响，不称为真实WinRT崩溃的完全确定性复现。

CoreHost Release构建通过，11/11相关原生套件通过（36.05秒）。独立审阅未发现阻断问题；补充了审阅提出的关闭异常详情。真实WinRT驱动的事件撤销/Close行为、并发多次停止以及历史崩溃是否完全消失仍需实机开始/停止循环验收。既有OnFrameArrived错误诊断本身可能抛出的边界缺口未在本轮改动，不把gate单测称为整个WinRT委托绝不抛出的证明。

证据位于out/cpu-memory-analysis/capture-lifetime-20261021/；原崩溃转储分析和MAP等价验证在descriptor-fix-field-test-20261020/。本轮只改原生采集关闭生命周期和原生测试，没有调整描述子、定位阈值、OCR显示策略或地区资源选择。

## 运行期字体像素副本回收（2026-10-21）

继续拆分资源保留约487MiB到探索约1GiB的增量时，发现另一块与地图特征无关的明确所有者：DX11字体图集上传后，CPU同时保留Alpha8和RGBA32。按实际15px完整中文字集、40px UI字集（含现有场景类别名称）、本机msyh.ttc独立构建：4096×4096，RGBA67,108,864字节（64MiB）、Alpha16,777,216字节（16MiB）、两份字体来源39,408,704字节（约37.58MiB）。ImGui实际live分配125,303,261字节（约119.50MiB），释放RGBA后58,194,397字节（约55.50MiB）。独立进程private从128,909,312到61,661,184字节，下降约64MiB。未把全部运行增量归因于字体。

仅在字体GPU视图创建成功、图集不含彩色像素且Alpha8仍在时，回收CPU RGBA并置空。保留所有字体来源、字形、度量、UV、Alpha8和GPU纹理。设备重建从原Alpha8展开RGBA再上传，不重新栅格化中文字库；彩色及RGBA-only图集保留唯一像素来源。正常帧无需RGBA，因此不在日常绘制循环重建，也未更改字体、字号、字集、输入钩子和首次字体预热顺序。

新增font-atlas-storage实机诊断，分别记录alphaBytes、rgbaRetainedBytes、rgbaUploadBytes、fontSourceBytes、glyphCapacityBytes；上传逻辑字节不等于CPU仍持有的RGBA字节，也不等于进程或GPU总内存。字体来源两份约37.6MiB仍保留，未在本轮改变其所有权。

红绿：产品原DX11 backend的5条像素持有断言RED，新实现GREEN。永久新测试使用实际ImGui和产品DX11 backend、无窗口WARP设备及自定义ImGui allocator，确认上传时live拥有量恰好少64MiB。GPU纹理读回、所有glyph/advance表逐字节对照；实际AddText/RenderDrawData绘制15px+40px中英文，连续10帧无RGBA重分配；3次Invalidate/Create后的纹理、文字渲染像素和几何均一致。彩色和无Alpha8图集不丢失来源，最终ImGui分配归零。HEAD原backend独立编译同测试为负控制，持有断言保持RED；旧/新atlasHash均17394923171921121514，renderedTextHash均15156769773180374645。没有修改既有断言。

WARP设备重建约29–38ms，旧版已保留RGBA时约9–13ms；新增开销只在设备对象重建，正常帧实测不触发RGBA展开，且首次构建仍在安装输入钩子之前。不把测试称为真实硬件设备丢失故障注入；GPU上传失败未做故障注入，既有backend失败处理未在本轮重构。

CoreHost与新字体测试Release构建通过；12/12相关原生套件通过（35.79秒）。独立审阅未发现阻断问题，之后补全了其建议的实际文字绘制/原backend对照。只修改原生代码，没有托管/XAML变化；没有减少地图候选、楼层、描述子或放宽定位规则。

停止探索后、刷新前的CoreHost快照另存quiescent-process-before.json；其残留占用不能全部认作字体，因为停止时ImGui context已销毁。本轮明确消除了活动会话64MiB冗余所有者；约500MiB运行增量的其余部分、基线地区按需加载仍待继续归因，不能承诺客户端总占用下降恰好64MiB或已达到600MB。

证据位于out/cpu-memory-analysis/font-memory-20261021/，包含实际字体分配探针、字体/GPU/文字读回红绿、原backend对照、构建与12项回归日志。刷新前后另核验全部载荷和地区激活/选择状态。


### 2026-10-21：帧账本实测与运动源紧凑存储

实测探索17:54:59–17:57:44，81条运行帧账本加1条停止样本；3708条JSON记录无解析错误、trackingFailures或frame-storage-error。唯一CPU图像底层分配运行中位95.898MiB、p95 113.025MiB、采样最大124.808MiB，分配器历史峰143.229MiB（峰值瞬间无分组快照）。WGC两份2562×1479 BGRA共28.909MiB；capture-delivery中位三份43.364MiB、最大五份72.273MiB；overlay-motion每份2048×1008 BGRA为7.875MiB，采样最多四份31.5MiB。viewport请求/锚点各最多一份7.875MiB。来源标签代表分配发生位置，不代表独立读者；浅拷贝和ROI不重复计数。停止探索后liveBytes=0、allocations=0，callbacksDrained=1、closeErrors=0。

实测基线约486MiB工作集/472MiB私有，活动稳定样本874–983MiB，历史工作集峰1213MiB。这些值不能减去账本值就认定剩余都是泄漏：账本不包含灰度、小图、GPU和其它运行库内存。输入最大间隔288ms（boundsMs=288）继续登记为后续停顿问题，本轮仍优先内存。原始证据out/cpu-memory-analysis/frame-storage-field-test-20261021/。

本轮将ScreenMotionTracker原有的最长边320、彩色INTER_AREA缩放提前到OverlayFrame发布阶段，保留motionSourceSize作为原始几何；灰度化、小地图彩色动态遮罩、拟合阈值和坐标换算不变。覆盖首锚点、历史源注册、fresh-source与recent-capture恢复；无尺寸元数据的旧完整图像路径继续可用。每份2048×1008 BGRA原8,257,536字节，现320×158 BGRA为202,240字节，减少8,055,296字节（97.55%）。四份同时存活时逻辑分配最多约节省30.73MiB；不能承诺进程工作集等额下降。

新增非连续ROI、三种通道、不同尺寸的真实底层容量与逐像素红绿对照，原始单位跟踪变换逐值一致；完整Overlay发布链路对照延迟定位与历史帧切换。连续性测试另以compact模式执行原fresh/recent恢复用例，保留原断言。账本一般仍统计>=1MiB BGR/BGRA，但显式纳入小型MotionImage，防止优化后落入统计盲区；小运动图计数断言先RED后修复。证据out/cpu-memory-analysis/motion-source-20261021/。原生修改，无托管或XAML变化。独立审查未发现阻断缺陷；全量回归及刷新核验结果见同目录最终日志与motion-source-deploy-20261021/。

本轮最终验证：CoreHost及受影响原生测试重新构建，20/20 CTest通过（47.37秒）；独立审查复核compact模式fresh/recent恢复测试无问题。全量首轮因三个未生成测试可执行文件未运行，随后生成并完整重跑全绿，未改动既有断言。测试树正常退出并整树脚本刷新、完整重启；398载荷differing=0，资源PRI、自包含检查通过。刷新前四份激活/选择文件不变；启动后测试激活指向新bundled快照，16包完整定义、地区选择和玩家状态保持。启动日志frame-storage-enabled含compactMotionIncluded=1，无warn/error/fatal。资源保留基线486MiB工作集/472MiB私有，与前轮相同；活动内存效果等待用户正常路线实测，不能把单份逻辑节省当作总进程降幅。核验out/cpu-memory-analysis/motion-source-deploy-20261021/。


### 2026-10-21：紧凑运动源实机验收

探索18:24:49–18:25:57共68秒，1621记录、33条运行账本及1停止样本。大地图运动源实际320×158 BGRA每份202240字节，小地图246×246 BGRA；不再出现完整2048×1008运动源。运动源分组中位0.193MiB/最大0.655MiB，前轮7.875/31.5MiB；总图像账本中位95.898→80.534MiB，历史峰143.229→127.672MiB。停止liveBytes/allocations/trackingFailures均0，WGC回调排空且关闭错误0，无warn/error/fatal或新dump。

总进程资源基线仍486MiB工作集/472MiB私有；10秒后定时样本866–945MiB，历史工作集峰1012MiB（前轮1213）。测试比前轮165秒短、路线负载不相同，前60秒工作集高度重叠，不把201MiB峰值差归因于单次优化，不认为已达600MB目标。global8次完成7次接受，中位891ms/最长1490ms；local96次完成75次接受，中位289ms/最长882ms。无6–12秒超长匹配。输入间隔最大15ms，本轮未复现288ms，间歇停顿仍保留登记。两个窗口共12次mapAttachRejected/trackingMisses，当时sourceReliable=0，不能称零跟踪丢失，也不足以确认像素跟踪回归。

下一优先项为完整capture-delivery图像的持有者/持有时长与重叠：中位三份43.364MiB，最多五份72.273MiB；WGC内部两份28.909MiB保持。先区分必需并存和可缩短的旧帧持有，再决定优化；继续拆解未纳入像素账本的运行增量。原始日志、可重复分析脚本与对照表：out/cpu-memory-analysis/motion-source-field-test-20261021/。


### 2026-10-21：完整截图读者与等待时长诊断

上一轮capture-delivery中位三份、最多五份完整截图并存，仅凭分配来源不足以决定哪份能释放。本轮继续拆解，先补诊断，不修改截图率、定位/显示/命中检测、图像所有权与释放边界。

代码核对：App::Start的latest/captured/currentSnapshot共同引用同一底层图像，生命周期跨LocalizationSignal等待；主循环无新帧时的提前等待亦持有latest。界面检测普通路径在try作用域结束时已经释放截图，再执行周期末sleep，不能算普通休眠持有；只有try内部的提前sleep分支仍持有。渲染截图局部作用域在主framePacer等待之前结束；PresentedOverlayFrame和GamepadCursorGeometry保留精确画面对应的像素用于命中，不能擅自删除。ReadoutVisualRequest/结果及readoutVisualAnchor仍可能保留完整截图。

扩展现有IMAO_FRAME_MEMORY_TRACE=1：分配获单调递增身份，不记录地址；每两秒frame-readers列出capture-delivery身份、底层字节、Mat引用数与分配年龄，以及已接入localizer/state-detector/renderer/readout-worker的帧号、底层身份、working/waiting、持有/阶段毫秒。定位与检测提前等待显式切phase。报告另列当前published/presented/cursor底层身份，用于连接必要的精确画面持有者。引用数是Mat/ROI头部共享数，不是线程数或shared_ptr读者数；别名不重复算字节。各发布通道与账本并非原子快照，日志显式atomicAcrossSources=0。

覆盖范围有限：readout-worker仅标记Compute执行期，不涵盖排队/待取结果及长期readoutVisualAnchor；未命名引用不能直接叫泄漏。ReadLease仅持有元数据，不持有Mat、shared_ptr<CapturedFrame>或像素，帧先释放仍安全，诊断异常计trackingFailures并由既有frame-storage-error显式报告。关闭环境开关不安装分配器也不建立读者表。当前/presented/cursor诊断读取使用const Mat&，避免额外浅拷贝虚增引用。

新增身份/别名一致性、引用数、持有时长、等待阶段、move语义、像素先释放、地址复用新身份、并发读者/释放及元数据故障隔离测试；先以空实现验证六项RED，随后实现。独立审查修正测试lambda捕获与报告临时Mat复制两项，复核无剩余阻断。证据out/cpu-memory-analysis/capture-readers-20261021/；构建/回归/刷新以该目录最终日志为准。

本轮最终交付：verified-build.log确认CoreHost与帧账本测试重新构建成功，verified-tests.log为20/20通过（49.52秒）。原生C++变更，无XAML/托管变化。按授权正常关闭测试客户端后以Update-MapTestBuild.ps1整树刷新并重启；398载荷differing=0、resources.pri一致、自包含通过。刷新前4份激活/选择文件不变；启动后16包完整定义、地区选择与玩家资源状态核验通过，测试激活正确指向新bundled快照。启动readerTrace=1，无warn/error/fatal。交付证据out/cpu-memory-analysis/capture-readers-deploy-20261021/。等待用户下一次探索/大地图拖动/切区/停止实测，依frame-readers判定可缩短的等待持有；本轮尚未宣称内存进一步下降。


### 2026-10-21：完整截图持有实测与下一修复边界

探索18:53:27–18:55:39约132秒，3046条记录、64个运行样本+1停止样本，无JSON解析/诊断/warn/error/fatal。定位线程48/64样本处于waiting，等待阶段中位35ms/最大82ms；32次所持截图与published/presented/cursor均不同，其中30次refs=3，吻合latest/captured/currentSnapshot三个Mat别名，共一份14.455MiB底层。非原子采样的另外两次refs=1，不作精确独占推断。下一步收紧定位处理轮的局部截图作用域，完成发布后在等待前释放，覆盖无新帧提前等待，保留异步及显示/手柄证据。潜在收益为避免一份旧图跨等待，并非三个别名合计43MiB，实际工作集降幅待实测。

完整截图2/3/4/5份并存分别12/24/26/2次，中位43.364MiB、最大72.273MiB。未命名分配中21条记录年龄>250ms、涉及10编号、单引用最长5.800秒，归属仍待查；规划绑定/手势、异步结果/锚点等路径未完整接入，不能直接定为泄漏。检测与渲染本轮仅被采到working且持有均<=7ms（瞬时抽样次数不代表实际执行次数）。

总图像账本中位80.534MiB、历史峰127.672MiB；10秒后工作集896–978MiB、历史峰1167MiB。本轮诊断不承诺总占用减少。停止图像与读者均清空，trackingFailures=0，WGC排空/关闭正常。global16次完成11次接受，中位786ms/最长2643ms；local238/203，中位211ms/最长842ms，无6–12秒超长匹配。输入间隔最大19ms未复现288ms，trackingMisses=5，仍保留偶发问题登记。证据out/cpu-memory-analysis/capture-readers-field-test-20261021/。


### 2026-10-21：定位循环等待前结束截图作用域

根据上一实测48/64次定位读者处于等待、其中32次持有非当前发布/显示/手柄截图的证据，本轮仅收紧App::Start每轮像素读取的词法作用域。latest、captured、currentSnapshot、相关临时Mat和读者诊断lease全部在真正LocalizationSignal::WaitUntil前销毁，避免已处理帧跨80ms左右周期等待。以waitDeadline和waitOnCaptures携带值类型调度信息出作用域，无新帧/空图的提前等待也经过同一边界。原计算deadline、capture/result唤醒策略及早退分支不重置startTime的语义保持。

已发布的紧凑运动源、异步请求、readoutVisualAnchor、显示精确画面和手柄命中持有不变。没有裁掉仍被使用的像素、重绑坐标时间或放宽定位/楼层门槛。仅能消除本轮局部别名的持有；其它读者持有同一底层时该图仍存活，不能承诺恒定减少14.455MiB，更不是三个浅别名43MiB。

本轮是作用域生命周期调整，沿用现有真实分配/读者释放、异步通知与坐标连续性回归，不新增仅重复局部实现的测试。实际等待旧帧数量/工作集收益以刷新后的实机frame-readers再次验证。修改前App.cpp证据、原生构建/回归日志及交付辅助脚本：out/cpu-memory-analysis/release-capture-before-wait-20261021/。

本轮验证与交付：CoreHost Release构建成功，20/20回归通过（38.13秒）；对照本轮修改前App.cpp的独立审查未发现作用域/协程/唤醒语义问题。整树脚本刷新并重启测试客户端，398文件differing=0，resources.pri及自包含检查通过；16包定义、地区选择与玩家资源状态保持，测试激活正常重定向新bundled快照。启动readerTrace=1，无warn/error/fatal。交付证据out/cpu-memory-analysis/release-capture-before-wait-deploy-20261021/，等待实机对照旧截图存活与工作集变化。


### 2026-10-21：等待前释放截图实测验收

客户端19:04:36启动，真正探索21:57:53–21:59:40约107秒，2572记录、52运行样本+1停止样本；不能算约3小时探索。无解析错误/warn/error/fatal/frame-storage-error或新dump。localizer waiting从上轮48/64次降至本轮0/52，working7次采样持有中位18ms/最大55ms；等待且不同于published/presented/cursor的截图从32次降0。验证定位等待前局部像素已释放。capture-delivery 2/3/4份并存分别11/29/12次，未采到5份（前轮2次）；瞬时采样不是完整执行次数。

图像账本中位80.534→80.438MiB几乎不变，采样最大127.672→103.056MiB，但历史峰仍127.672MiB，不能宣称恒定省14.45MiB。10秒后定时工作集864–914MiB、中位889，前轮896–978；进程历史工作集峰1167→1104MiB，测试时长/路线负载不同，不将63MiB差全归因本次修复。资源基线仍486/472MiB。停止图像及读者表归零、trackingFailures=0、WGC关闭排空正常。

仍有8个未命名单引用旧图年龄>250ms，最长7.130秒；下一步优先核对规划绑定/手势及异步结果/锚点来源，停止会释放，不定为泄漏。global8次完成7接受，中位999ms/最长1889ms；local235/214，中位216ms/最长887ms。输入间隔最大37ms（trackMs37/boundsMs5）未复现288ms；小地图source-already-attempted共4次trackingMisses并有130ms短缺口，以context-ineligible结束，登记后继续优先内存。证据out/cpu-memory-analysis/release-capture-before-wait-field-test-20261021/。


### 2026-10-21：转查剩余内存大项（旧截图暂缓）

按用户要求暂停7.130秒单引用旧截图，顶部优先级已更新。主要常驻资源逻辑可计算约321.16MiB（描述子143.56、关键点31.40、索引84.21、词典2、楼层59.98），已包含于486MiB工作集基线，不重复相加。活动截图账本中位80.44；其余有定位展开/索引、OCR、CPU字体55.5、D3D/驱动/堆，以及现场填充未知的48MiB灰度LRU。最新大地图matcher缓存实测47.93MiB，3072×1512 SURF检测工作区124.05MiB是瞬时大项。跨进程/不同时间/不同度量与有包含关系的数值不能凑成总账。

新增离线memoryProbeReleaseHistograms诊断：全资源完整校验后释放17.564697MiB直方图vector，6730瓦片同query分数逐值一致；单进程WS少17.566/private少17.602MiB；控制/实验各8项原回放failed=0。ScoreTiles仅消费postings等，不读histograms，但文件校验/合并/保存仍需要原格式，生产落地应在可变构建阶段处理，当前仅诊断实验。

新增out目录font-only R8探针：真实4096²纹理RGBA8→R8，12帧文字读回hash一致，ImGui最终归零。本机硬件GPU local少48MiB、private少51.625MiB，WS仅少4.016MiB。没有图标/彩色atlas/设备重建全面验证，原型不能部署；不把GPU/private下降当WS下降。另两份同源字体文件共37.58MiB，理论共享差18.79MiB；当前ImGui flag=false会clone，不能只改flag实现共享。

建议后续：低风险运行直方图17.56MiB可先落地；更大常驻方向是143.56MiB编码描述子按包按需加载/有界缓存并保留全部检索覆盖；峰值方向是SURF124.05MiB检测工作区进一步分带复用，必须保留现有全尺度/特征/接受结果。字体R8偏向GPU/private，常驻WS优先级较后。楼层样本/停止态释放仍暂缓。完整证据、归因边界与候选排序out/cpu-memory-analysis/remaining-memory-20261021/readings.md、summary.json及graphics/。本轮只改诊断工具/本地探针，无产品优化刷新，不承诺600MB。


### 2026-10-21：运行时直方图副本释放落地

用户批准继续后，先实施已离线验证的17.564697MiB直方图副本释放。RuntimeFeatureRepository完成所有区域文件校验、合并和FinalizeMergedVisualIndex后，在发布shared const资源前调用ReleaseBuildOnlyIndexData；visualIndexReady=false时不清理，重复调用返回0。仅释放histograms vector容量，postings/postingOffsets、tiles（含histogramNorm与原偏移）、featureRows、vocabulary、关键点与描述子保持原所有者。文件codec/builder/磁盘格式不变，不能在文件校验和合并之前删除载荷，也不能把精简运行索引当完整索引重新序列化。

新增visual-index-runtime-storage事件记录实际released/retained histogram字节；预计全14地区18417920/0，实际启动日志为准。替换此前离线const_pointer_cast试验为Repository发布资源容量断言memoryRequireRuntimeIndexCompacted，实验const cast不进入产品。

新测试覆盖未就绪不清理、实际容量释放而非仅clear、检索分数逐值一致、其它检索所有者/全局行号不变与幂等。先以空实现观察两条容量断言RED；新增fixture的FLANN词典构建会消费OpenCV全局RNG，故只在新测试内保存/恢复RNG状态以隔离旧用例，没有修改既有断言。既有codec用例仍检查完整文件校验/损坏拒绝。构建/20项回归及实际全地区8项回放证据见out/cpu-memory-analysis/release-index-histograms-20261021/最终日志。

当前先交付这一项；143.56MiB编码描述子按包按需加载涉及全局行号与资源生命周期，留作下一阶段设计，不删候选、不降低检索覆盖。7.130秒旧图、楼层样本和停止态释放仍暂缓。

本轮最终验证/交付：CoreHost、OptimizationTests、VisualRegression Release构建通过；20/20回归通过（39.25秒）；实际Repository发布后的histograms size/capacity均0，全部8项全地区既有回放failed=0。独立审查确认无发布后histogram payload消费者，无Repository索引序列化路径，加载线程独占清理且失败不发布。整树刷新并重启测试树，398文件differing=0，自包含与resources.pri通过；4份激活/选择状态刷新前不变，启动后16包完整定义、地区选择与玩家状态保持，测试激活正常retarget。

22:26:11真实CoreHost启动事件visual-index-runtime-storage为histogramBytesReleased=18417920 histogramBytesRetained=0；resource-load-retained wsMB=468/privateMB=454（前轮486/472），与逻辑释放17.565MiB相符，活动探索总内存效果仍待实测。启动无warn/error/fatal。核验辅助脚本发现PowerShell将JSON ISO日期自动转日期后再Parse会丢失偏移，筛入此前离线探针日志；已改为先在原始ISO字符串上按偏移筛选再解析JSON，最终启动证据仅含本次CoreHost。交付证据out/cpu-memory-analysis/release-index-histograms-deploy-20261021/。本轮只改原生C++，无托管/XAML变化。

### 2026-10-21：合并描述子改用已校验只读文件映射

用户要求继续下一阶段，和直方图副本释放一起实机验收。本轮在所有区域载荷完整校验、合并及索引就绪后，把合并描述子写入本机临时文件，Flush并逐字节读回比对后建立PAGE_READONLY映射，再替换原heap Mat。现有IMF压缩文件不能直接随机映射，因此没有改变包格式或绕过解压校验；编码、全局行号、候选、位置与楼层接受门槛不变。该实现让系统按页驻留与回收冷数据，不是固定容量缓存或按区域禁用，也不消除初次加载解压峰值。

映射所有权接入OpenCV UMatData，自定义allocator只管理映射；普通分配转发默认allocator，保留原账本。Mat别名/ROI延续文件和映射寿命，最后一个读者跨线程释放也关闭全部句柄并删除文件。随机名CREATE_NEW和DELETE_ON_CLOSE原子创建，避免先创建再打开的异常退出残留窗口。失败保留原heap数据，显式descriptor-storage-error，不静默放行；IMAO_DESCRIPTOR_MAPPING=0仅供对照。资源就绪和活动诊断区分logicalBytes与QueryWorkingSetEx得到的residentBytes，另列failures/closeErrors，不能把映射虚拟字节当已消除的数据或全额工作集节省。

单元覆盖uint8/float、非连续ROI、原始字节、跨编码边界逐行解码float bits、实际候选去重/展开、PAGE_READONLY和MEM_MAPPED、别名/ROI跨线程最后释放、失败保持输出及临时文件清理；空实现已有RED证据。CoreHost/VisualRegression/新单元最终构建成功；21/21回归通过（38.33秒）。独立审查无阻断问题，原子创建建议已落实。新测试引用实际候选子集头后需windowsapp.lib，补齐链接依赖，没有修改原断言。

同一更新后二进制、同一全14地区快照，分进程顺序执行heap控制与映射实验，均保留直方图释放：各8/8回放通过，scene/坐标/角点/检索分数及瓦片数、接受/拒绝结果一致。资源就绪WS325.39→183.42MiB，private320.66→177.30；回放后WS384.79→320.94、private378.29→238.12MiB。逻辑描述子仍150538240字节（143.56MiB），冷启动resident=0，回放后81866752字节（78.07MiB）。首次小地图374→358ms，原图646→620ms；噪声拒绝2931→2781ms，部分缩放略有波动，有限样本不能推断普遍提速或冷磁盘表现。最终mapped bytes/mappings/failures/closeErrors及Mat账本全部归零。证据out/cpu-memory-analysis/mapped-descriptors-20261021/的final-build.log、final-tests.log、comparison.json和两组jsonl/report。

仍暂缓7.130秒单引用旧截图、楼层编码、停止态缓存释放及已记录光标停顿。广泛跨区会增加驻留页，磁盘冷页访问可能增加延迟，不能承诺全地区恒低于600MB。接下来整树刷新并重启测试客户端，合并验收两项优化。

本阶段最终交付：Update-MapTestBuild.ps1正常关闭旧测试客户端、整树同步并重启；398文件differing=0，自包含及resources.pri通过。刷新前4份资源状态不变，启动后16包完整定义、地区选择和玩家状态保持，测试激活正确retarget。23:02:54本次真实启动日志确认直方图18417920字节释放/retained0，描述子mode=mapped logicalBytes=150538240 residentKnown=1 residentBytes=0，映射准备elapsedMs=312。resource-load-retained WS325/private311MiB，对照上一阶段468/454各少143MiB；加载峰WS660MiB仍在，不能当成活动探索内存。readerTrace=1，启动无warn/error/fatal。CoreHost SHA256 7DA8F9E3A172B3424172BCF45BA77E26A1F35E1EBF8EBF96D2EBA0E93EC75FA6；交付证据out/cpu-memory-analysis/mapped-descriptors-deploy-20261021/。原生改动无托管/XAML变化。等待用户探索/大地图/跨区/停止一轮，合并验收直方图释放与映射，核对resident、总WS/private及首访耗时；不以冷启动325MiB承诺活动<600MB。

### 2026-10-21：直方图释放与描述子映射合并实测

本次仅筛选23:02:50.481+08启动请求之后日志，3336条事件，真正探索23:05:19–23:07:53约154秒。76份活动映射/帧样本、75份overlay样本，无JSON解析失败、warn/error/fatal、descriptor-storage-error/frame-storage-error；mapping failures/closeErrors及帧trackingFailures均0。启动直方图18417920字节释放、retained0，描述子mapped logical150538240、resident0；准备312ms，基线WS325/private311MiB。两项在本轮正常生效，可通过当前覆盖场景验收。

探索10秒以后20份定时采样WS740–837MiB、中位785，private766–822、中位791.5；历史WS峰1068MiB。前轮等待前释放截图实测WS864–914、中位889、历史峰1104，但路线/时长不同，不能把104MiB中位差全部归因本阶段。描述子resident从活动初期11.14MiB到末尾115.87MiB，中位76.27；逻辑仍143.56MiB，末尾仅约27.69MiB描述子未驻留。最后数份驻留相同但有限路线不能证明长期有界总工作集；映射支持OS回收冷页，不是固定预算缓存。私有提交减少与总工作集下降不是同一指标，不能把325MiB冷基线当活动值；本轮活动并未低于600MiB。

389次大地图结果371接受。global8/7，中位994ms、最长1185ms；local381/364，中位47.8ms、最长710ms。接受sceneId覆盖1/2/6/9，不等于覆盖全部14区域。未出现6–12秒长尾；本轮无映射引入明显首次匹配退化的证据，但没有相同现场路线heap对照，不能证明普遍不变。75份overlay采样inputGap中位9/最大24ms、mapBuildMax最高5.675ms，未复现此前光标长停顿，不能承诺永不复发。

图像账本中位80.34MiB、采样最大103.06MiB、历史峰126.49MiB；localizer waiting采样0。停止后图像/读者表归零，WGC callbacksDrained=1/closeErrors=0。描述子映射仍1份是应用级资源缓存预期保留；session-stopped residentKnown=0表示该报告未查询资源，不是驻留页清零，也不能据此判定泄漏。用户仍暂缓停止缓存、7.13秒旧图和楼层编码。证据out/cpu-memory-analysis/mapped-descriptors-field-test-20261021/（events.jsonl、Analyze.ps1、summary.json、runtime-memory.json、descriptor-residency.json、slowest-matches.json）。本次只分析记录，没有改产品代码或再次刷新。

### 2026-10-21：运行时索引无损紧凑表示

用户批准继续削减约66.65MiB常驻索引。本轮仅更换发布前运行表示：原10479891条uint32行号约39.98MiB，按瓦片保存zigzag差值varint（有符号差值保留降序、重复、0与uint32最大值）；原2302240条12字节posting约26.35MiB，运行时省去由postingOffsets确定的重复wordId，仅存tileIndex和原float权重。所有磁盘codec/builder/完整文件校验和shard合并仍使用原格式；完整索引就绪后构建独立紧凑所有者，逐行/逐posting核对后才释放原featureRows及postings容量，失败显式日志并保留原数组。转换不改变瓦片、全局行号、候选覆盖/预算、排序、重复项、权重、词偏移、定位/楼层门槛。

原生RuntimeIndexStorage提供逐瓦片解码访问，现有查询直接向原有临时候选vector追加或mask去重，不建立全局展开副本。RuntimeFeatureResources适配未压缩fixture与诊断禁用路径；CompactRuntimeIndex未ready拒绝、发布前唯一所有者执行、重复调用幂等。IMAO_RUNTIME_INDEX_COMPACT=0仅作同二进制对照。默认路径失败runtime-index-storage-error显式记录，Build局部所有者异常销毁后旧源保持；资源停止取消在转换前后检查。

TDD先对空Build运行观察validated index compacts断言RED；编码测试覆盖空瓦片/非连续行/重复/降序/uint32极值、实际容量降低、全部行顺序、浮点位与源释放后复制保活、无效范围/偏移拒绝。新增运行资源测试比较ScoreTiles完整double分数向量逐值一致，核对两份原容量归零、未ready拒绝和幂等，没有更改既有断言。独立审查发现四个VisualRegression诊断模式仍直读旧数组，已迁移VisitTileRows并保留预算/querySize/截断语义；复核无剩余问题。

最终构建、全量回归、实际全14地区资源顺序控制/紧凑对照及交付结果，以out/cpu-memory-analysis/compact-runtime-index-20261021/及compact-runtime-index-deploy-20261021/为准。7.13秒旧截图、楼层样本、停止态缓存和光标停顿继续暂缓。

最终对照结果：CoreHost/OptimizationTests/VisualRegression及新单元Release构建通过，22/22回归通过（38.65秒）。同一二进制顺序运行heap控制与packed实验，两组均保留先前直方图释放和描述子映射；8/8实际回放通过，scene/坐标/角点/检索TopScore/瓦片数/接受拒绝逐项一致。真实原行号41919564字节压到10719369（含瓦片范围元数据），posting27626880→18417920；合计少40409155字节（38.54MiB），常驻索引66.65→28.11MiB。

离线资源就绪WS183.32→142.73MiB/private177.18→136.50；回放后WS319.84→280.62/private235.37→195.38；差值含分配器布局变化，不与38.54MiB逻辑减少混称同一度量。首次小地图374.86→364.22ms，原图644.85→646.12ms，噪声拒绝2889.15→2780.79ms，其它缩放最高约+19ms；有限样本无明显退化，不承诺所有场景提速。映射owner及Mat账本最终全部归零。修复后的--viewport-storage另以实际测试树资源加载冒烟成功，9个scene均有非空候选/正常matcher容量，共40,850,728字节；不是空查询通过。没有改变既有断言或磁盘文件。最终代码审查复核确认四处诊断迁移解决、没有新增发现。comparison.json及viewport-storage-smoke.log保存独立证据。

最终整树交付已完成：按持续授权正常关闭测试客户端后Update-MapTestBuild.ps1刷新并完全重启，398文件differing=0，自包含与resources.pri通过，刷新前4份激活/选择不变，启动后16包定义/地区选择/玩家资源状态核验通过。23:25:52真实CoreHost事件mode=packed，originalRowBytes=41919564 originalPostingBytes=27626880 packedRowBytes=10719369 packedPostingBytes=18417920 elapsedMs=50；直方图释放及描述子映射继续生效。resource-load-retained WS287/private272MiB，上一阶段325/311；初次加载峰WS670仍在，压缩不消除解压合并峰。readerTrace=1，启动无warn/error/fatal。CoreHost SHA256 89A927B7777DDF20D1EC0474B874BA55F4F03D6C72B2723EF7745774115709AC。交付证据compact-runtime-index-deploy-20261021/。原生C++修改，无XAML/托管修改。等待同路线探索/拖图/跨区/停止实测活动内存及延迟，尚未宣称低于600MiB。本轮未提交/push/发布。

### 2026-10-21：紧凑运行索引实机验收

本轮仅读取23:25:48.302+08启动请求之后3495条事件；探索23:28:26–23:31:10约164秒，80份帧/驻留/overlay样本，无JSON解析失败、warn/error/fatal或索引/映射/帧诊断错误。启动确认mode=packed、原row41919564/posting27626880→packed10719369/18417920，转换50ms；逻辑减少38.54MiB。直方图释放和描述子映射仍正常。加载后WS287/private272，探索前283/268MiB；原生算法无本轮新修改。

探索10秒以后20份定时采样WS704–881MiB、中位728.5；private730–835、中位764。上一轮WS740–837/中位785、private766–822/中位791.5。定时采样前30秒更密，中位数是采样中位，不是时间加权中位。路线负载不同（本轮接受scene1–7共7种，前轮1/2/6/9共4种；global23次对8次），不将56.5MiB样本中位差完全归于本次优化，也不宣称全程下降。本轮后半程90/120/150秒WS881/857/875MiB，历史峰1093高于前轮1068；常驻优化没有消除跨区缓存增长和瞬时峰。

描述子驻留中位113.36MiB、末尾/最大130.44MiB（前轮76.27/115.87），逻辑仍143.56MiB，仅约13.13MiB末尾未驻留。它解释部分跨区增长，不能解释全部工作集变化。图像账本中位80.53MiB、采样最大105.54MiB、历史峰136.84；localizer waiting采样仍0。

289条viewport-result里有2条预测冲突/次帧确认事件，不当独立计时搜索；真正带scope计时287次，global23/17接受、中位891.6ms/最长2644.3ms，local264/222接受、中位150.5ms/最长862.3ms。两次global未接受耗时2644.3和2518.2ms，均budgetExhausted=1并各尝试2组计划；记录为预算边缘耗时，不因低于6秒忽略，也没有相同现场heap对照可证明与解码完全无关。仍未出现6–12秒长尾。80份overlay inputGap中位9.5/最大20ms，mapBuildMax最高4.445ms，未复现光标长停顿；抽样不是完整延迟分布。

停止后截图和读者表归零，trackingFailures=0；WGC callbacksDrained=1/closeErrors=0。应用级映射仍保留1份，session-stopped residentKnown=0为未查询，不是resident归零。当前覆盖可接受紧凑存储正确性和常驻收益；活动仍远未低于600MiB，下一步峰值/其它常驻项需另行量化。旧图7.13秒、楼层、停止缓存和既有光标停顿仍暂缓。本次仅分析记录，未改产品或刷新；证据out/cpu-memory-analysis/compact-runtime-index-field-test-20261021/。

追加峰值来源线索：23:30:34 viewport-query-storage记录4096×2016查询，detectorScratchBytes约220.52MiB，超过先前3072×1512约124.05MiB的工作区。该值为检测器逻辑临时工作区，已包含于进程总占用，不能额外相加或直接视作可省220MiB。下一轮若优先压峰，可从这一实际大尺寸/缩放路径做对照，保持尺度与检测结果。

### 2026-10-21：SURF Hessian trace改存符号

继续优先内存，拆解实测4096×2016 detectorScratchBytes=231235460（220.52MiB）：原积分图33,054,596字节，加三层float determinant和三层float trace共198,180,864。代码唯一trace消费者仅在maxima中心以(trace>0)-(trace<0)生成关键点class_id，不读取trace幅值或相邻trace；因此保留原float dx+dy及正负比较，仅把三层trace保存为CV_8S符号-1/0/+1。determinant仍float，三邻层旋转/各octave尺度/阈值/NMS/插值/排序/描述子/取消与调度保持，无降采样或特征裁剪。零、负零和NaN比较语义仍为0。

新检测工作区4096×2016为156917636字节（149.65MiB），理论及实测少74317824（70.875MiB，32.14%）；图像、描述子、关键点和并行开销仍不在该逻辑计数内。高密度大噪声301467点的描述子阶段会另成峰：实际额外Mat峰231235460→187405832字节，只少41.80MiB，不能承诺进程峰必定下降70.875MiB。此项不改变空闲资源基线，活动全程节省需实测。

TDD保留既有所有断言，增加实际分配上界而非仅修改计数，加入4096×2016的非连续ROI/噪声/空白/棋盘格输入；旧实现新增两条内存界限断言RED，新实现关键点全部字段/顺序和描述子每字节与stock SURF完全一致，并通过原取消、回调异常、竞争OpenCV全局并行guard与所有Mat释放用例。独立审查完整trace读写/轮转/边界/线程语义无阻断问题。旧源/测试证据保存在out/cpu-memory-analysis/surf-trace-sign-20261021/，不修改既有测试阈值。

改动前VisualRegression先跑同一当前全地区manifest控制，再构建改后CoreHost/工具与单元、全回归、改后回放；最终结果以该证据目录build/tests及control/optimized jsonl/report为准。原生C++修改，无XAML/托管修改。继续保留旧截图7.13秒、楼层和停止缓存暂缓项。


最终验证：CoreHost/VisualRegression/StreamingSurfTests Release构建成功，22/22回归通过（40.76秒）。同一全地区快照，改动前二进制先跑control、改动后跑optimized（不引入产品双模式）；各8/8回放通过，坐标/角点/scene/matches/inliers/检索分数/瓦片数/接受拒绝均完全一致。原图659.21→651.59ms、噪声3032.18→2839.64ms、1.25缩放737.22→764.19ms，有限样本波动不推断普遍提速。实际Mat区间峰原图152.12→141.35MiB、1.25缩放197.29→180.47MiB；一些小图/噪声由其它缓冲峰主导，不能逐项等额减少。进程历史WS峰532.39→533.05MiB几乎不变（资源加载主导），本轮不宣称整进程峰已经降低70.875MiB。comparison.json保存字段一致性和分阶段账本。

最终整树交付：正常退出旧测试客户端后Update-MapTestBuild.ps1刷新并重启，398文件differing=0，自包含/resources.pri通过，4份激活选择刷新前不变；启动后16包定义/地区选择/玩家资源状态保持，测试激活正确retarget。23:41:02本次CoreHost启动无warn/error/fatal，readerTrace=1，直方图释放、索引packed与描述子mapped生效；WS286/private271MiB（上轮287/272，符合本项不减空闲基线）。CoreHost SHA256 BCD2E651DED917BF3CB1119CA8BB8C865D9345A80C7B60CF87E81C75EF0B1CF6，启动PID1512，UTC请求2026-10-21T15:40:57.9620320Z。交付证据surf-trace-sign-deploy-20261021/。接下来按实际跨区/拖图/缩放路线检查大尺寸查询scratch及整进程峰；没有把逻辑70.875MiB直接当进程收益。本轮未提交/push/发布。

### 2026-10-21：SURF trace符号存储实机验收

筛选23:40:57.962+08启动请求之后3179条事件；探索23:42:26–23:44:44约138秒，67份帧/驻留/overlay样本，启动基线WS286/private271、探索前283/267MiB。无JSON解析失败、warn/error/fatal、索引/映射/帧诊断错误；trackingFailures及mapping failures/closeErrors均0。真实3次4096×2016查询detectorScratchBytes均156917636（149.648MiB），对照上轮同尺寸231235460（220.523），确认精确减少70.875MiB；3次3072×1512均84.182MiB，对照原124.049，少39.867MiB。4096查询detect124.82/122.17/129.52ms、describe256.75/337.80/353.58ms，图片不同不能推断运算加速。

进程历史WS峰1093→1022MiB，降低71，但本轮138秒/8种接受scene（1–7、9），上轮164秒/7种，路线/持续时间不同，不能把71全部归因trace改动。实际检测器临时容量下降可确认，整进程峰下降有本轮观测支持，非严格同路线因果对照。探索10秒以后19份定时采样WS700–925、中位753（前轮704–881/728.5），private740–882/中位774（前轮730–835/764）；定时采样前段更密，不是时间加权中位。后程90/120秒WS867/925，说明持续占用仍高，不能把压峰称作全程降内存。

描述子resident中位123.52、末尾136.64MiB（原143.56逻辑只剩约6.92MiB未驻留），前轮113.36/130.44；跨区后接近全驻留，仅解释部分持续增长。图像账本中位80.34、采样最大105.34、历史峰119.80MiB，前轮80.53/105.54/136.84；不将路线差解释为本次直接优化截图。localizer waiting样本0。

249条viewport-result含4条预测冲突/次帧确认，真正计时245次：global20/16接受，中位940.97/最大2701.95ms；local225/191，中位187.72/最大801.38ms。3条失败global约2702/2359/2132ms均budgetExhausted=1，仍记录预算边缘失败，不宣称耗时问题全解决；没有6–12秒长尾。67份overlay inputGap中位10/最大20ms，mapBuildMax最高6.112ms，未复现长光标停顿。停止后截图/读者归零，WGC callbacksDrained=1/closeErrors=0；映射仍为应用级保留，停止报告residentKnown=0是未查询而非清零。

本轮覆盖内trace存储压峰通过验收，持续内存/其它缓存仍需继续拆解；不承诺低于600MiB。旧图7.13秒、楼层、停止缓存继续暂缓。本次仅分析记录，无产品代码修改或刷新。证据out/cpu-memory-analysis/surf-trace-sign-field-test-20261021/（events、summary、runtime-memory、descriptor-residency、large-surf-queries、slowest-matches）。

### 2026-10-21：补齐跨区持续内存的Mat与灰度缓存现场账本

用户要求继续拆解持续占用。本轮先补调查盲区，不砍缓存、不改定位/渲染算法、不追暂缓旧图。此前frame-storage仅跟踪>=1MiB BGR/BGRA及紧凑运动图，未覆盖灰度LRU、楼层/浮点候选和词典等；48MiB灰度容量只能当上限，不能算已占用。沿用IMAO_FRAME_MEMORY_TRACE=1，在同一allocator元数据表增加可选allMatrices统计：拥有底层的2D Mat>=64KiB（含全部已跟踪帧例外），浅拷贝/ROI只计底层一次。原frame-live/groups/readers仍限定frame，通用Mat包含帧，不能相加。默认构造/关闭环境开关保留原行为，映射描述子绕过普通分配器，仍由原驻留事件单列。

mat-storage每两秒及资源retained/停止/关闭时列独立live/peak/allocation、完整type容量总数、前32个origin/尺寸/type形状与omittedShapeBytes；logical分配不等于resident/private，也不涵盖小Mat、STL、模型专用allocator、字体、GPU/堆空洞。DenseMapConfirmer新增只读锁内缓存容量getter，不复制Mat、不改变LRU；denseCacheBytes含于通用Mat，缓存和Mat快照非原子，日志标明包含及atomicAcrossSources=0。没有用账本差额称泄漏。

TDD空统计实现新增断言RED后实现。新增灰度/float/color唯一底层、尺寸类型、ROI保活/跨线程末尾释放、真实峰/归零、小对象阈值、frame读者接口排除census-only、小运动图包含及元数据故障隔离测试；沿用原断言。灰度现有fixture新增实际tile容量及warm调用无重复载入检查。审查发现Observe/Identity需限定frame，以及小运动图低于64KiB需保持总账包含关系，均修正并补测试。元数据异常显式trackingFailures/frame-storage-error，不影响已成功像素分配。

本轮会构建回归后整树刷新，下一次探索/拖图/跨区/停止实测才可定量回答活着的灰度/浮点Mat有多少；不会把新增诊断当作内存优化收益。证据out/cpu-memory-analysis/resident-mat-census-20261021/。无托管/XAML修改，旧截图7.13秒/楼层/停止缓存继续暂缓。

最终验证与交付：CoreHost/VisualRegression/OptimizationTests及FrameStorageAccountingTests构建通过，22/22回归通过（verified-tests.log，40.59秒）。独立审查两项边界问题修复后复核无新增阻断。整树脚本刷新并完全重启，398文件differing=0，自包含/resources.pri通过，刷新前4份状态不变，启动后16包定义/选择/玩家资源状态保持。跨午夜验证脚本改为按原始ISO时间过滤启动当日及以后events文件，避免固定events-20261021遗漏10-22启动；没有通过变更产品来迁就验证。

00:00:20新启动mat-storage基线live64850432字节（61.85MiB）、190个大Mat，type0 37019648/type5 27830784，主要楼层数据及2MiB词典；小于64KiB漏计载荷仍由原资源容量表补充。denseCacheBytes=0/entries0，trackingFailures=0，源映射resident0；WS286/private272MiB，启动无warn/error/fatal。不能把61.85再加进286基线，也不能把灰度预算48MiB当启动实际占用。采集元数据开销存在但仅既有诊断开关启用，不宣称新增内存节省。CoreHost SHA256 C00D8D9C5A7670FF21BB1423928D77D7FD6197BBFB6F8AEDDA94C9B78F4B46F5，PID36092，UTC启动请求2026-10-21T16:00:15.7684800Z（本地10-22零点）。下一轮仅读此次之后日志，量出跨区灰度/浮点缓存实际存活，再与native提交/工作集对照。交付resident-mat-census-deploy-20261021/，基线mat-baseline.json。未提交/push/发布。

### 2026-10-22：大Mat账本首轮实测，定位停止态额外74.14MiB

仅筛00:00:15.768+08启动之后日志3490条；探索00:03:37–00:05:57约140秒，68份活动Mat/帧/overlay样本。无解析错误、warn/error/fatal、帧/映射/索引诊断错误，trackingFailures=0。新Mat账本源去重/类型/形状正常，可用于调查。

启动大Mat61.846MiB，停止135.982MiB，净多74.136MiB；类型差值精确分为CV_8U +10MiB（Dense缓存10张1024²，独立getter也为10MiB）和CV_32F +64.136MiB。灰度缓存不是48MiB满预算，不是当前最大疑点；停止截图/帧读者全0。停止态普通Mat仅浮点和灰度，不把74.136直接称为泄漏，因为定位器缓存设计会跨探索存活，用户也暂缓停止态清理。应优先核对额外64.136MiB浮点的具体所有者及重复展开，寻求共享/延迟展开而非直接砍预算。

形状线索：128×12296 float有3份独立底层，合计18.012MiB；128×32193有15.72MiB；另有128×15166约7.41、128×12634约6.17MiB，词典形状128×4096由启动1份变3份。形状相同不等于内容相同，不能直接认定三份是相同候选重复副本；现有大地图OwnedFlannMatcher本身已共享候选Mat，不能重复提出已做过的取消其拷贝。FeatureRowCache和其它候选保持浮点，需下一步增加所有者关联或独立生命周期对照。当前没有足够身份/内容证据逐项归因64.136MiB。

活动大Mat186.01–266.70MiB、中位228.81，已包含彩色图像中位80.53，不能再相加；float中位99.62包含楼层及词典基线，不全是候选。灰度缓存活动0→10MiB后维持10。映射描述子另计（普通allocator不跟踪），resident中位111.625/末尾137.215MiB。普通Mat是逻辑底层容量，映射值是resident，不能混成严格WS总账；STL/KD树、OCR专用模型分配、字体/图形/运行库/分配器保留页仍未全部覆盖。

探索10秒后19个定时采样WS682–935/中位807MiB，private705–886/中位792；历史WS峰1021。与前轮路线不同不推因果退化，新诊断没有声称减少内存。真正计时global22/16接受、中位961.46/最长2045.21ms；local186/144、中位113.67/最大774.51；另3条非scope计时事件分开，不把确认事件计成独立搜索。inputGap最大23ms，mapBuildMax6.892ms，未出现长光标停顿和6–12秒匹配。停止WGC drained/closeErrors与映射均正常。

已生成out/cpu-memory-analysis/resident-mat-census-field-test-20261022/events.jsonl、summary.json、mat-breakdown.json、runtime-memory/descriptor-residency/slowest-matches。此轮只读分析和记录，没有修改产品、清缓存或刷新。旧截图7.13秒/楼层编码/停止缓存仍暂缓。

### 2026-10-22：为额外64.14MiB浮点矩阵标记实际分配来源

依据首轮大Mat实测，停止比启动多74.14MiB，其中10MiB明确为灰度缓存，另64.14MiB浮点不能只凭尺寸命名。本轮继续调查，新增minimap-candidates、viewport-candidates、minimap-vocabulary、viewport-vocabulary四类origin。Scope只包住FeatureRowCache的UniqueFeatureSubset、大地图SelectSceneCandidates及两个词典索引创建，异常/返回恢复原origin；不改候选、缓存预算、排序、RNG、距离或生命周期。全局词典member通过C++20保证消除拷贝的prvalue lambda在Scope内直接构造，资源所有者先构造后释放次序保持。大Mat日志新增完整owners字节分组，在top32形状截断前汇总；每个底层只归属一个分配标签，不是shared_ptr读者数或独占归属证明。

已核对本机OpenCV源码third_party/src/opencv-4.11.0/modules/flann/src/miniflann.cpp，Index::build内部features_clone=_data.getMat().clone；两定位器索引均从2MiB词典构建，符合现场词典尺寸1份→3份，多4MiB的线索。仍不把同尺寸3份128×12296当相同内容；优先获得带标签的实际字段。

VisualRegression memory-ledger在创建既有账本前按环境启用FrameStorage，形成ledger→census→默认allocator链，单独的原账本与新census释放都验证；逐viewport结果报告owner容量，完全释放资源/定位器/楼层后报告及断言census归零。普通未开开关时不额外安装分配器。新增诊断include提前Windows头触发min/max宏污染OpenCV affine.hpp，已在工具入口定义NOMINMAX修复，未修改第三方或测试断言。独立审阅Scope/构造/释放链/全部owner汇总无阻断问题。

最终构建/22项回归/实际全地区8项回放与owner数值见out/cpu-memory-analysis/float-owner-tags-20261022/。本轮仅补归属证据，不宣称内存节省；旧图7.13秒、楼层编码与停止缓存仍暂缓。

最终实验证据：CoreHost/VisualRegression/FrameStorageAccountingTests/OptimizationTests构建成功，22/22回归通过43.30秒。开启Frame census的实际8项全地区回放均通过；与前轮既有report比较scene/坐标/角点/matches/inliers/检索TopScore/瓦片数/接受拒绝全部一致。8次结果owner总和均等于该次大Mat live，四个目标owner均非空。离线minimap-candidates稳定40263168字节（38.40MiB），viewport-candidates19736576–20903424（18.82–19.94MiB），两vocabulary各2097152（共4MiB）。这不是现场64.14MiB的逐项精确归因，因为路线/缓存填充不同；已证明来源可区分且主要候选展开方向可信。所有定位器、资源、楼层销毁后census live0，原MatMemoryLedger live0，mapping bytes/count/closeErrors0，所有者链正确归零。

独立审阅无新增问题。owner-readings.json、verification.json、probe-events/jsonl/report保存数值与完整释放证据。后续应优先检查小地图FeatureRowCache能否保留编码行并按需读取，同时验证CPU和距离数值不变；不直接删缓存、不承诺64MiB全部可省。若要确认现场三份同尺寸是否同内容，仍需以标签现场样本或行身份进一步验证。

最终整树刷新并完全重启已完成，398文件differing=0，自包含/resources.pri及16包定义/选择/玩家状态核验通过。00:29:02真实启动owner baseline=other:64850432、trackingFailures0，WS287/private272MiB，无warn/error/fatal；先前直方图/packed索引/mapped描述子继续生效。本轮仅诊断归属，无新增内存收益。CoreHost SHA256 A30434FD620929A483F1B1CDD1AF4D93AFE0729BB8BED5BF00AAC3A69186C96F，PID31964，UTC请求2026-10-21T16:28:57.6202942Z。交付float-owner-tags-deploy-20261022/。下一现场按该请求筛events-20261022，核对停止态四owner合计与64.14MiB差值。未提交/push/发布。

### 2026-10-22：浮点所有者现场实测完成，停止态增量完全对账

仅筛00:28:57.620+08启动请求之后5153条记录；探索00:31:28–00:34:41约193秒，95份活动mat/overlay/帧样本。无解析/警告/错误/帧或映射诊断失败，trackingFailures0。新owners保留完整形状分组并与live一致，可用于归因。

启动普通大Mat61.846MiB，停止157.032，增量95.186MiB（上一轮74.136，不同路线缓存填充不同）。本轮停止差值可精确对账：minimap-candidates58.833 + viewport-candidates21.353 + minimap-vocabulary2 + viewport-vocabulary2 + 灰度缓存11 =95.186MiB；浮点部分合计84.186MiB，无额外未命名float增量。灰度getter13个条目但实际11MiB（空条目不占图像载荷），不能按13张计13MiB，也未填满48MiB。启动资源基线不重复相加。

活动小地图候选中位78.049/最大78.550MiB，大地图中位23.668/最大48.631MiB；活动小地图词典4MiB、停止2MiB，大地图词典2MiB。标签统计实际底层总量，不是单个cache受管容量，不能因78.55>64就断言预算失效。源中VisualRuntime持久engine与VisualTrackingContext独立engine都创建自己的FeatureRowCache/词典，活动上下文及被淘汰读者可能同时存活；停止时一份小词典释放与少一份引擎一致，标签本身未按engine实例区分。下一主目标是小地图候选float缓存的无损编码/按需读取，需保持候选去重顺序、float距离与几何结果，测CPU和延迟；不是直接砍预算或仅停止后清缓存。未认定同尺寸矩阵内容重复。

活动定时采样WS629–934/中位758MiB，private680–874/中位802，历史WS峰1061；映射resident中位127.797/末尾136.234MiB，不能把普通Mat逻辑容量与resident/WS混成相加总账。真正计时global25/17接受，中位1003.70ms、最长3467.84ms；local223/168，中位98.48/最大731.23ms；余16条非scope/确认或状态结果不作独立搜索。最长global失败，budgetExhausted1、7组含1483ms wide计划；3.47秒预算超出记录保留，不当成6–12秒长尾回归或忽略。inputGap最大32ms，mapBuildMax6.892ms，未复现旧光标长停顿。停止帧/读者归零，映射无错误；普通Mat保留是当前应用级缓存策略，不直接叫泄漏。

证据out/cpu-memory-analysis/float-owner-tags-field-test-20261022/的events、summary、owners-breakdown、runtime-memory和slowest-matches。此轮仅检查记录，无产品修改/刷新。旧图7.13秒、楼层编码、停止缓存清理仍暂缓。


### 2026-10-22：小地图候选缓存保留编码，匹配时有界解码

用户批准后实施。FeatureRowCache生产路径保留原8bit描述子，混合scale/offset区间按候选输出行重排；去重仍使用原浮点值与坐标，浮点来源兼容。缓存按原float-equivalent字节计费，保留相同64MiB准入/淘汰集合，另记实际stored容量，避免以压缩后容量装入四倍候选。统计读取与修改统一在mutex内。匹配继续共用cv::batchDistance及原L2/双向/tie扫描，仅解码当前批；128列SURF最多2MiB解码+约2MiB距离缓冲，不展开整个缓存。IMAO_MINIMAP_ENCODED_CACHE=0可同binary切回float供A/B，默认编码。大地图FLANN没有改为编码。

先加RED回归：旧实现的编码存储、有界缓冲、缓存编码命中三条断言失败；再实现并GREEN。补充混合编码重排序、非连续ROI、去重、每一位距离/索引/tie、首批后取消、后段编码异常、所有scratch归零、512query总scratch<=4MiB，以及相同LRU淘汰/读者存活和float兼容。CoreHost、OptimizationTests、VisualRegression、StreamingSurfTests构建通过，最终22/22回归通过40.45秒；独立只读审查无阻断，建议补强的取消/上界测试已补。没有修改既有测试断言。

同binary交替顺序合成计时（4 OpenCV线程，2次warmup+12次有效，每组取中位）：1024/8192/32768候选×36/128/512query。编码匹配增加解码成本，8192行多约0.54–1.26ms，32768行多约1.80–4.75ms；36query/32768行8.17→10.94ms，相对34%，不能声称距离内核完全不变快慢。描述子载荷严格为float的1/4，实际总缓存另有keypoints/行键/编码段，不能直接说总缓存减少75%。

最终真实全地区资源同binary顺序fresh process A/B，3次相同小地图结果（质量/歧义/不完整/候选所有几何分数）逐项相同；float耗时368.15/40.78/20.13ms，encoded367.57/42.91/19.00ms。此样本未见整次定位明显回退，但非所有现场路线保证。8项大地图回放均通过，除时间外全部报告字段一致。两模式定位器/资源/楼层销毁后原Mat账本和mapped字节/个数/closeErrors均归零。

候选填充、取消未销毁engine时：普通Mat总量107541120→77256192字节，减少30284928（28.882MiB）；这是同一资源/回放下的总量差值，主要来自候选描述子编码；同点private199380992→168812544、WS267329536→236593152，单次进程计数仅作辅助。历史WS峰558669824→558612480几乎不变，说明本轮不降低由此前资源/楼层加载产生的该离线峰值，不能把28.88MiB称为峰值降幅。实际现场两engine与路线不同，需要本次测试树验收。证据out/cpu-memory-analysis/encoded-minimap-cache-20261022/的red/green/final-build/final-ctest/benchmark、final-float/final-encoded.jsonl与report、verification.json。

另完成只读原生内存采样器out/cpu-memory-analysis/native-census-20261022/：核验owned测试CoreHost的CIM与handle路径，按MEM_PRIVATE/MAPPED/IMAGE聚合commit/resident/private/shared，列映射basename与最大40个私有分配，无地址/载荷/trim/debugger。首次目标已退出明确记unavailable；重启后再采实际基线。此工具统计虚拟分配容量，尚不能直接给出堆中活对象或泄漏结论。旧图7.13秒、楼层编码、停止缓存清理仍暂缓。


本轮交付与原生基线：整树398文件differing=0，自包含/resources.pri核验通过，16包定义、测试选择及玩家状态保持。首次直接启动因提权未继承trace导致旧Verify-Startup失败；该脚本还写死上一轮全地区数值，失败证据startup-verification.json/startup-log.json保留。没有修改产品测试断言。用Restart-Traced.ps1提权辅助程序仅正常关闭/重启已核验owned测试UI/host，显式继承trace；新Verify-CurrentStartup按实际资源选择核验mapped/packed/直方图释放和owner基线，current-startup-verification全部通过，无warn/error/fatal。新启动UTC2026-10-21T17:26:57.2166023Z（本地10-22 01:26:57），UI12124/CoreHost38384，CoreHost SHA095792F2FA4BE58D8CAB0452DE501D158748179C4175A83A5E566EB509C01E98。当前选择下描述子映射12114048字节，启动普通大Mat8654720字节，与离线全地区不是同一资源负载，不能据此把差值归为本轮压缩收益。未提交/push/发布。

只读原生实采01:27:36完成约33ms，核验owned CoreHost38384；完整地址遍历，2317区域、2100批次，working-set/counter失败0，未查询字节0，Errors空。WS169.691MiB、private-usage156.738MiB。MEM_PRIVATE committed141.359/resident136.168MiB；MEM_IMAGE committed240.309/resident33.043MiB；MEM_MAPPED committed17.090/resident0.449MiB。模块映像较大的commit不等于物理占用，paddle_inference映像resident17.879MiB；不能把整个映像commit当成可省工作集。最大六个private分配组约94.812MiB committed，暂未识别其活对象/子系统，不命名为OCR或泄漏。20个mapped-name查询失败1006明确保留，未知映射resident0.3125MiB；页面驻留查询本身均成功。不同OS计数语义与非原子时点不混为精确加总。证据native-census-20261022/encoded-startup.json与encoded-startup-summary.json。

源核查发现DLL_API.cpp现有core-baseline在DrawItemBase::Initi之后；后者已经加载标记JSON、markerIdentities以及完成状态/刷新路由索引。因此历史“基线”不能直接解释成只有模块/运行库。后续私有内存归因应优先在标记载入/身份索引初始化前后独立计量，再对OCR初始化与探索会话做同样分段；仅此快照不能证明94.8MiB全部属于标记数据。当前交付可开始现场验收编码候选缓存，活动期峰值与匹配时延仍待实测。


### 2026-10-22 上午：编码候选现场验收，诊断开关丢失与输入间隔异常记录

本轮验收：2026-10-22 11:24:58启动至11:27:18，2991条记录，解析/警告/错误0。凌晨01:26:59另一次启动与该轮分离，不能混算。当前owned test CoreHost13032已核验路径，文件SHA与本轮交付095792F2FA4BE58D8CAB0452DE501D158748179C4175A83A5E566EB509C01E98一致。

全地区资源指纹与上次现场相同：mapped logical150538240、直方图释放18417920、packed row10719369/posting18417920；启动retained WS286/private271MiB。上午重启未继承IMAO_FRAME_MEMORY_TRACE，详细owner/frame-reader/cache stored日志缺失，所以无法从这轮实证描述子1/4存储或准确节省28.88MiB；不能伪造owner对账。已交付binary默认编码，但该轮也没有启用时mode日志，模式切换不作额外现场证据。

活动定时>=10秒：19份，WS757–833/中位796MiB，private737–789/中位753；全部memory事件历史WS峰943。上次对应峰1061/WS中位758/private中位802，本轮峰低118MiB但WS中位高38、private中位低49，地区路线/缓存填充不同，不能把这些差额全归压缩。活动memory事件点也有SURF临时样本，不能只把19个周期点当成绝对峰值。

274条map-viewport-result中266条实际计时（global19/local247），8条确认/状态不独立计搜索。global18/19接受，中位915.27ms/P95 1441.40/最大2147.07；local211/247接受，中位61.93/P95 404.66/最大547.38。上次global中位1003.70/最大3467.84，local中位98.48/最大731.23；本轮未见6–12秒旧长尾，也没有看到可辨认的整次匹配回退。最长11:27:09 global失败budgetExhausted1，六组多尺度计划，962ms最后组导致软预算超出，保留记录。小地图global11条，Strong3/Rejected8，其中7条incomplete350–382ms，不能说全部定位成功；小地图local26条21接受，中位4.30/最大11.80ms。没有逐帧ground truth，接受数不等于定位精度证明。

光标：62份overlay，inputGap中位11/P95 16/最大285ms，其余最大18ms。异常11:25:29，boundsMs285、track11/present7，mapBuildMax2.146ms；整轮mapBuildMax5.977ms，未见旧标记构建长阻塞证据，但不能宣称完全无卡顿。bounds覆盖窗口/渲染目标同步及NewFrame等准备，日志不能精确定位到某个WinAPI或排除调度暂停。保留input-gap-outliers.json，按用户此前指示继续内存优先，不本轮顺手更改光标路径。WGC close callbacksDrained1/closeErrors0，stagingFailures0。

停止探索后11:29:14只读native快照：WS628.805/privateUsage429.848MiB；驻留private394.535、image92.078、mapped142.156，其中描述子文件映射139.270MiB。完整3362区域/2703批次、working-set与counter失败0；27个mapped-name失败1006保留，不能隐去未知映射。不同OS计数时点/语义不能把privateUsage与resident相加；这张是停止态，不代表运行峰值，也不能拿凌晨单地区startup直接减去当泄漏。源raw在native-census-20261022/encoded-field-stopped.json。后续拆解优先仍是private对象（标记JSON/索引、OCR、缓存），已确认139MiB源描述子页属于映射驻留。

完整证据out/cpu-memory-analysis/encoded-minimap-cache-field-test-20261022/（保留原所有新启动记录all-events.jsonl及本轮events.jsonl、summary、slowest-matches、input-gap-outliers、Analyze.py）。本轮仅分析、只读快照及记录，没有产品改动或刷新测试树。


### 2026-10-22：私有内存分阶段计量，标记初始化与独立OCR

本阶段仅分解计量，不改变候选/匹配/显示或内存释放策略。标记初始化发生在资源preload之前，逐阶段普通进程privateBytes/WS计数无需环境开关；原core-baseline位置不变以保持历史可比。新增marker-init-before、逐scene JSON、JSON ready、ledgers、identities、routing、init-ready，以及727类别/24109点位/24109身份索引、18699刷新ID计数。RecordMemory增补raw wsBytes/privateBytes/peakWsBytes，API失败写memory-error，handlesKnown显式表示句柄计数是否成功。

真实客户端11:41:58启动：marker-init-before private57.176MiB（不是空CRT，仅表示进入标记初始化前）；JSON ready91.734（+34.559）；ledgers94.332（+2.598，含刷新分类）；identities115.676（+21.344，含refreshablePointIds）；routing118.305（+2.629）；init-ready128.852（+10.547，混合完成状态迁移与路由服务初始化，未单独命名为路由图）。总增量71.676MiB。每区域JSON计量中World19,187点增26.605MiB，其余相加约7.95；它们是累计private commitment增量，含分配器保留与日志成本，不是精确DOM堆占用，也不保证可一比一释放。不能将marker-init-before全算模块/运行库，或把这些数与上一进程驻留直接加减当完整对账。

身份索引每点保留JSON对象（sceneName/nameId/stateId/pointId）及字符串key；它与原始标记JSON同时存活。已定位21.344MiB阶段和对象数量，可作为后续紧凑身份结构的优化候选；不在本阶段修改索引。源码既有category.value("location", json::array())会产生临时副本，已记录但未改动。

OCR补充init-before及YOLO构造/预热完成（Paddle分支区分model与warmup）里程碑。宿主OCR与捕获/定位并发，进程前后差不能当OCR独占。故在现有IMaoCoordinateYolo工具增加--memory独立进程probe：同一生产recognizer、48x892空白输入，加载+构造自带预热、20次推理、释放模型四点，毫不改动recognizer源码/参数。三次独立试验private增量：ready36.66/37.55/38.70MiB；20次后45.05/46.32/46.72；释放后剩4.14/3.82/3.97。WS增量分别ready46.25–46.93、20次后51.27–51.91、释放后6.50–6.82。该量含ORT/DLL/session/工作区，不是仅权重；固定空图不是所有真实帧的峰值，释放后有crop和分配器缓存，不能说剩余4MiB是模型泄漏。

独立只读审查发现probe中途输出失败未检查，已在每次flush后检查流状态并显式抛错；最终probe构建/一次完整正向/无效报告路径负向(exit1)通过。没有新增Diagnostics到YoloCoordinateRecognizer，保持独立tool链接边界。CoreHost/VisualRegression/Optimization/CoordinateYolo构建通过，22/22回归通过46.60秒；既有断言未修改。此类低影响计量以实际阶段样本和现有回归验证，未加镜像实现的单测。

整树自动刷新且正常关闭/完全重启owned测试客户端；398文件differing0，自包含/resources.pri、16包定义、4份测试/玩家状态哈希不变。提权helper继承IMAO_FRAME_MEMORY_TRACE=1与IMAO_MEMORY_TRACE=1；启动无warn/error/fatal/memory-error，各6个关键marker阶段唯一且rawbytes有效。UTC2026-10-22T03:41:55.6922446Z（本地11:41:55），UI8040，CoreHost SHA256=1A6575F101F842C254DD05D419E8152DFF81CFAAE3FE739079E381B6875CBA89。当前全地区描述子150538240、直方图释放18417920、packed与原先一致，普通大Mat64850432保持。尚未开始本阶段游戏内OCR/探索验证，不虚构其数据；测试树已可验收。旧285ms bounds段停顿、旧图7.13秒与楼层编码/停止缓存仍留待，继续内存优先。未提交/push/发布。

证据out/cpu-memory-analysis/private-startup-census-20261022/的before快照/build/final-build/ctest/ocr-isolated-summary/summary/assessment，以及private-startup-census-deploy-20261022/的marker-memory-milestones、marker-counts、current-startup-verification与verification。


### 2026-10-22 中午：私有阶段诊断现场与编码缓存验收完成

本轮日志按11:41:55.692+08交付启动之后筛选，仅1个CoreHost启动（11:41:58），探索12:01:35–12:03:23，3038条。解析、warn/warning/error/fatal、memory-error与帧/映射/索引诊断失败均0。frame/owner详细trace与IMAO_MEMORY_TRACE生效，minimap-candidate-storage mode=encoded budgetCharge=float-equivalent明确。此前标记阶段计量仍完整。

编码缓存实证：最后命中22entries，float-equivalent held43853648（41.822MiB）、真实stored12902456（12.305MiB），差30951192（29.517MiB）。包含keypoint/rowkey/编码段容量，非仅描述子；不是整个进程节省的控制A/B。停止minimap-candidates普通Mat10317184（9.839MiB）均编码type0，源码同源浮点描述子载荷会4倍，和上述总缓存差额相符。不同计量覆盖（Mat不含vector、最低64KiB阈值）不强行混为同一数字。

停止普通Mat110012800（104.916MiB），启动64850432（61.846），增量45162368（43.070）精确归于：小地图候选9.839 + 大地图候选28.231 + 两个词典各2 + 灰度缓存1MiB。owners总和与live一致，trackingFailures0。活动Mat周期采样最大约228.7MiB（含帧、请求、候选、缩放，另有mapped源）；帧采样最大102.671MiB、生命周期peak127.672；停止frame live0、读者/published/presented/cursor均0。WGC callbacksDrained1/closeErrors0，映射failures/closeErrors0。常驻cache与当前资源留存不据此称泄漏。

整进程：18份>=10秒周期采样WS673–994/中位713MiB，private703–797/中位744.5；精确历史WS峰1184501760字节（1129.629MiB），首次在12:03:05 local-matcher-built报告看到，无法凭该报告点认定峰就由新建该20MiB matcher导致；历史峰可能在前次采样间隔内发生。上轮峰943/WS中位796/private中位753，不能只选中位改善声称峰下降。不同路线、缓存填充、代码页驻留不同，本阶段仅诊断，没有新增内存优化。每viewport-query-storage detectorScratch最大23842564（22.738MiB），没有把历史4096x2016的149MiB代入本轮实际数据。

OCR实际初始化12:01:35：进程private560087040→643489792，增79.539MiB，WS482082816→593702912增106.449；此时capture/匹配/首次定位并发。结合上阶段独立probe纯模型链20次后约45–47MiB，不能把现场79.5全算OCR。YOLO ready，Paddle初始化里程碑没有出现。小地图global3条（Strong2，ambiguous拒绝1，无incomplete），最长73.889ms；local24条21接受，中位5.848/最大11.786ms。

大地图238结果中232条计时（13global+219local），其余6条确认/状态。global中位947.965/最大1519.919ms，接受10；local中位195.916/最大722.584，接受193。无6–12秒长尾。local整体中位比上轮61.934高，但样本构成差别大：accepted local scene1本轮132条中位216.2ms，上轮57条184.2；scene9本轮29条中位256ms，上轮没有；上轮scene3有82条中位46.5、本轮没有。不能把混合中位直接称编码解码回退，也不能声称所有匹配更快。若后续查CPU应按场景和查询大小/缓存miss分组。

53份overlay：inputGap中位10/P9518/最大278ms（12:02:28），bounds278、track11、present2、mapBuildMax2.399；该行hooks=mouse=off keyboard=on，因此不能从此条推出它实际阻塞鼠标hook。整轮mapBuildMax5.066，无旧标记构建长阻塞证据。窗口/新帧准备段异常重复记录，尚不能确定单个API或排除调度暂停。按此前指示暂不扩大停顿工作，继续内存优先。

停止后原生只读采样private-census-field-stopped.json成功：完整遍历，无working-set/counter错误；27个mapped-name失败1006保留。WS754.332/privateUsage439.277MiB；驻留private405.270、image226.723、mapped122.305。前轮停止WS628.805、private驻留394.535、image92.078、mapped142.156，所以这次停止WS增加主要在image页，不能一概说私有堆持续涨。

新大头线索：paddle_inference.dll resident116.039（其中private-resident0.508MiB）、mkldnn.dll resident45.230（private-resident0.102），合计161.270MiB且绝大部分是共享映像页；本轮业务使用YOLO，现有IdentifyWorldCoordinates仍保留Paddle分支并链接相关依赖。值得后续核查是否可把旧OCR依赖从宿主常驻链隔离，但这是独立设计/兼容性决策，未删除分支/卸载DLL/承诺省161MiB私有内存。共享代码页与Task Manager私有内存口径不能混淆；本轮1129.6是进程WS历史峰，包含共享页。

本轮仅读日志、补只读原生快照、保存分析，无产品修改、刷新、commit/push/发布。原始events/all-events与summary/owners-breakdown/slowest-matches等在本目录；native原始快照在native-census-20261022/。下一步候选仍有紧凑身份索引约21MiB阶段，以及新发现的旧OCR依赖常驻链；实际收益须分别验证。


### 2026-10-22：Paddle依赖延迟加载，保留旧OCR能力

本阶段用户同意继续核查旧OCR常驻链。实际PE普通imports只有paddle_inference.dll为根，mkldnn/mklml/libiomp来自其传递依赖；YOLO业务本身动态加载独立onnxruntime。参考Microsoft MSVC delay-load文档（https://learn.microsoft.com/en-us/cpp/build/reference/loading-all-imports-for-a-delay-loaded-dll），采用现有链接器延迟加载而非拆新子系统：CMake函数imao_delay_legacy_ocr对CoreHost、OcrDependencyTests、CoordinateRuntimeTests应用delayimp.lib与/DELAYLOAD:paddle_inference.dll。CoreHost原plain target_link_libraries改PRIVATE以免混签名，库集合不变；未删除旧识别分支/第三方库/模型/交付文件，未引入卸载逻辑，不改变匹配/OCR参数、门槛或队列。CoordinateRegression旧OCR工具仍保持原加载方式。此阶段不实施身份索引压缩。

先加RED：新IMaoOcrDependencyTests未调用OCR时GetModuleHandle已见Paddle/mkldnn，按预期exit1；control private53800960/ws67985408。保存before CMake及control CoreHost/test二进制。新链接选项GREEN，实际PE中Paddle仅位于delay imports、普通imports无Paddle/MKLDNN/MKLML/libiomp；程序加载及YOLO真实推理都不载入Paddle/mkldnn。旧PP-OCRv5在后台线程首次构造，随后另一线程Run成功，销毁预测器后模块保持加载，避免导入指针/仍有工作者时卸载。新测试要求旧OCR非空文本及有限正confidence。Control与delayed合成图都得到120,240,3及同置信度；3个已存正式608x33游戏ROI样本的完整YOLO结果（包含相同拒绝）及旧OCR text/score逐项相同。正式样本case0 private初始省49.16MiB、YOLO后48.99；case30省49.44/49.27；case100省49.14/46.71。只属于对应隔离进程，不承诺共享映像驻留161MiB等于private收益。

最初从旧640x128采样带手工裁的ROI位置有偏差，YOLO拒绝；CoordinateRuntimeTests原断言因此失败(runtime-test.log)。保留错误ROI、依赖输出及失败日志，没有改测试去迁就。改用已有coordinate-flight-intake-20261016/inputs.json正式ROI后，同一原始异步RuntimeTests通过（含队列像素所有权、多分辨率/BGRA/非连续帧、失败模型路径及关闭重启），runtime-official-fixture.log/runtime-verification.json记录exit0。独立审阅发现尺寸断言本身偏弱，已补非空/有限confidence与销毁后模块仍加载断言；未发现其他代码阻断。末次23/23 CTest通过41.89秒，CoreHost、新依赖test、CoordinateRuntimeTests构建成功。控制旧模型和Paddle DLL均使用本地已有资源，无下载。

错误路径范围：缺DLL/导出时delay helper产生SEH，现有std::exception catch与noexcept CRNN接口不能提供平滑降级。此前eager完整依赖缺失也无法正常启动，现在只是坏依赖的失败时点迁移；本改动不宣称支持损坏/缺少旧DLL包。保持原完整载荷和校验。未新增运行库（delayimp是静态链接辅助库）。

完整测试树自动正常关闭/整树刷新/重启，398文件differing0，自包含/resources.pri、16包定义、4份测试/玩家资源状态哈希核验通过。UTC2026-10-22T04:26:12.4717215Z（本地12:26:12），UI11740/CoreHost21044，CoreHost SHA256=FB748D267011628692C3BBEFF14180AB749EBDC3D0CDACA831EE83602DDF5865。基础marker/rawbytes及详细frame/cache开关继承；当前启动无warn/error/fatal/memory-error。全地区源描述子150538240、直方图释放18417920、packed索引和普通Mat64850432均保持原值，未通过少加载区域获得收益。

真实宿主对比上一阶段同全地区启动resource-load-retained：WS302096384→242974720，减少59121664（56.383MiB）；private285974528→234946560，减少51027968（48.664MiB）。marker-init-before private59953152→6164480，marker-init-ready135110656→84201472，说明避开的旧依赖初始化确有private开销，不仅共享DLL页。不同启动期分配器布局仍有轻微波动，不能把每个阶段差直接当精确live heap释放量。

启动只读native快照完整成功，WS231.766/privateUsage224.027MiB；MEM_PRIVATE/MAPPED/IMAGE遍历完成，working-set/counter错误0，16个unknown-mapped-name错误1006保留且没有unknown-image。未见Paddle/mkldnn/mklml映像，与PE及隔离动态测试一致。尚未本轮游戏内探索/YOLO接管实测，不宣称活动峰值按56MiB或161MiB下降，也不承诺600MiB目标。测试树已可验收。旧278/285ms窗口准备间隔、身份索引/楼层编码/停止缓存等仍待，未扩范围。未commit/push/发布。

证据out/cpu-memory-analysis/delay-paddle-20261022/的RED/GREEN、eager-control/official与delayed/official、real-comparison、runtime日志、ctest、core-imports、verification，以及delay-paddle-deploy-20261022/启动/状态/marker里程碑；native原始快照native-census-20261022/delay-paddle-startup.json。

新依赖回归同时接入scripts/Test-Runtime.ps1的构建与执行列表，以及Build-ReleaseCandidate.ps1原生构建目标列表，避免只登记CTest而正式门禁漏建。两脚本PowerShell语法解析通过，新target已实际构建/执行并进入23项CTest。未运行完整发版或托管发布流程；此处脚本列表改动不改变已验收CoreHost载荷。


### 2026-10-22：Paddle延迟加载现场验收完成

本轮验收对应12:26:12.471+08启动，唯一CoreHost启动12:26:14，探索启动2026-10-22T12:56:09+0800至12:58:22，3562条记录。解析、warn/warning/error/fatal、memory-error与已有storage诊断错误0，详细trace可用。源码/启动交付已确认Paddle仅delay imports。

正常路径Paddle占用问题已获得现场验证：12:56:16 OCR YOLO model-warmup-ready、ocr-initialize ready1；跑完停止后只读扫描同一owned测试CoreHost21044（与本轮启动一致），paddle_inference/mkldnn/mklml/libiomp均未见，完整地址遍历/working-set/counter错误0且没有unknown-image。Paddle一经显式调用delayhelper不会自行卸载，所以当前进程运行后仍未加载与未走旧分支一致。正常YOLO路径避开旧Paddle DLL加载及初始化开销；磁盘文件/兼容代码和delay导入表仍保留，只有显式旧分支会加载，不表述为Paddle相关磁盘/二进制字节全部消失。不会在YOLO失败时自动回退Paddle。

启动resource-load-retained仍为上一交付WS231/private224MiB，全地区资源/包指纹不变。19份>=10秒周期样本WS593–825/中位684MiB，private611–773/中位704。工作集历史峰958MiB（summary保留raw精确字节），上一轮1129.629；下降不全部归因Paddle，路线和缓存/代码页驻留不同。可确定的同全地区启动private收益48.664MiB见delay-paddle-20261022/verification.json。本轮OCR并发窗口private622010368→663519232，约39.586MiB，接近独立模型范围，但仍是进程总量而非纯模型独占。

停止后native WS567.383/privateUsage393.645MiB，驻留private369.082、image64.277、mapped133.988；上一轮停止754.332/439.277与image226.723，映像驻留差与旧链未加载相符，但不同路线不作精确总账A/B。仅CoreHost计数，不包含WinUI与Launcher，不能称全应用已经低于600MiB。native原始delay-paddle-field-stopped.json保留unknown-mapped-name失败码/数量，驻留查询无错误。

停止帧live0、published/presented/cursor0、readers none；Mat112171136（106.975MiB）对startup64850432增45.128MiB，可按owner对账：minimap编码候选13.162 + viewport浮点候选22.966 + 两词典各2 + 灰度缓存5MiB。所有owner之和等于live，trackingFailures0；cache填充不同于上一轮43.070MiB增量，不称泄漏。描述子仍映射150538240字节；停止residentKnown0不当成驻留0。

匹配：192条大地图结果中190条计时（22global/168local），另2条确认/状态；global中位818.913ms/最长2278.210（该次失败budgetExhausted1，21267查询点、多尺度组1202ms，保留软预算超出），local中位78.096/最长895.589ms；无6–12秒旧长尾，不声称所有分位都更快。小地图global4次、local157次153接受，local中位5.783/最大10.924ms。没有标注真值，不把接受数等同精度证明。

65份overlay inputGap中位9/P9518/最大300ms，12:56:48 bounds300/track9/present1，mouse=on；同窗mapBuildMax2.150ms。另12:56:31 mapBuildMax26.017ms对应inputGap33/track33，独立于300ms段，较此前5ms最大值高，已保留，不说完全没有marker长帧。窗口准备段旧异常仍存在，继续按用户内存优先约束记录，不在本轮顺便修复。WGC callbacksDrained1/closeErrors0。

本轮只分析日志、采只读native快照与记录，无产品变更/刷新/commit/push/发布。继续优化候选仍为标记身份索引及JSON常驻，Paddle正常常驻链的这次变更已通过本轮现场验证；活动峰值958MiB说明尚未完成全应用600MiB目标。

证据out/cpu-memory-analysis/delay-paddle-field-test-20261022/（events/all-events、summary、assessment、Analyze.py及各耗时明细），原生快照native-census-20261022/delay-paddle-field-stopped.json。

### 2026-10-22：标记身份索引紧凑化交付，待游戏内验收

将 DrawItemBase 的 unordered_map<string,json> 身份副本替换为 MarkerIdentityIndex：保留相同 state:point 键、哈希及插入顺序，每点只存 group/state，727 个分类共享自持有 scene/category 字符串。pointId 从第一个冒号后恢复；JSON 仅在完成命令归一化与同步导出时构造。原始标记 JSON、刷新路由、refreshable 集合、图像匹配与 Shutdown 行为不变。无借用原始 JSON 引用；重复键最后写入生效，正 state 筛选/非正全选及四字段协议保持。

新测试先运行 stub 得到 identity count RED，再实现 GREEN。小样本覆盖未知键无修改、重复、负/零 state、空 ID、冒号、中文、内嵌 NUL、无效 group 拒绝、临时字符串所有权及 Clear 后导出所有权。真实九份目录 24109 点逐项对照旧索引的命令归一化和各地区导出数组（含顺序）通过。只读审查无阻断；两项补测建议已落实。CoreHost 实际 Release 构建通过；24/24 CTest 通过（44.39 秒）。新 target 已接入 Test-Runtime 构建/执行及 Build-ReleaseCandidate 构建列表，修改脚本语法解析通过，未运行发布流程。

同真实目录各三次新进程测量：legacy 索引阶段 private 增量21.36328125MiB，compact4.08984375MiB，少17.2734375MiB；WS阶段增量少16.58203125MiB。包括分配器余量及同样循环临时复制，非精确 live 对象字节。verify 模式同时构造两种索引，仅用于行为对照，不参与内存计量。

已正常关闭 owned 测试客户端/子宿主，脚本整树刷新并重启。398 文件 differing0，自包含及 resources.pri 核验通过；16 包定义及4份测试/玩家资源状态刷树前后不变，启动激活正确重定向到新测试快照，玩家资源状态与选择保持。UTC2026-10-22T05:18:22.0336430Z 启动，UI23932，CoreHost SHA256 E5F8FD4E45DCE16BD4B138A7028C6278937C82924455A55927F2FB1AE3E8A578。启动 mode=compact groups727/identities24109/refreshable18699，警告/错误0；描述子映射150538240、直方图释放18417920及普通Mat64850432保持，非少加载区域获得收益。

真实宿主相对前一阶段同全地区启动：marker-init-ready private84201472→65703936，少17.640625MiB；resource-load-retained private234946560→215191552（224.0625→205.22265625MiB），少18.83984375MiB；WS242974720→223989760（231.71875→213.61328125MiB），少18.10546875MiB。独立索引对照约17.27MiB支持主要收益来源，但启动布局/瞬时分配有波动，不能把18.84MiB全部解释为精确对象释放。此阶段不改匹配算法；尚未现场确认活动峰值、完成/同步交互或导出耗时，不宣称游戏内匹配更快或全应用600MiB目标达成。旧窗口准备停顿继续记录待办。未commit/push/发布。

证据 out/cpu-memory-analysis/compact-marker-index-20261022/（RED/GREEN、final-build、ctest、catalog-parity、legacy/compact各三轮、host-comparison、Deploy/Verify）；compact-marker-index-deploy-20261022/（载荷/启动验证、marker里程碑与计数）。测试树已就绪，待用户现场验收。


### 2026-10-22：紧凑身份索引现场日志复核

本轮日志对应紧凑身份索引交付，13:18:24唯一宿主启动，13:20:09进入探索状态采样至13:25:19停止，14627条记录、解析失败0、warn/error/fatal及storage诊断错误0。测试树CoreHost哈希与交付E5F8FD4E...E8A578一致；启动mode=compact groups727/identities24109/refreshable18699。现场只验证启动/绘制/探索没有新异常；没有明确完成/同步成功事件，不能仅由无错误宣称这些交互现场验收覆盖。
工作集历史峰910.1015625MiB，25份>=10秒周期样本616–789/中位660MiB；私有提交618–754/中位662MiB。上一轮峰958.109、中位684，方向下降，但路线/缓存不同，不将全部48MiB峰差归因身份索引。该改动可靠隔离收益仍约17.27MiB，启动private224.06→205.22MiB。
停止后13:28:43只读快照核验owned测试CoreHost35124，WS533.777/private351.945MiB；驻留private331.305、mapped134.641、image67.797。上一轮567.383/393.645，不构成严格同路线A/B。地址遍历完成，working-set/counter错误0，无unknown-image；23次mapped-name失败1006明确保留。仍无Paddle/mkldnn/mklml/libiomp映像。
停止帧live0/readers none，WGC callbacksDrained1/closeErrors0。普通Mat97.937988MiB比baseline61.846191增36.091797：minimap候选14.554199+viewport15.537598+两词典各2+灰度缓存2MiB；owner账相符，trackingFailures0。停止缓存保留不是本轮新泄漏证据。
172条大地图结果，global10次中位1000.564/最大1589.736ms；local162次中位215.766/最大797.604ms；相比上一轮local中位78.096更高，场景/缩放不同，不能说匹配更快或证明回归，当前身份索引改动未触及匹配计算。无6–12秒长尾。小地图local1956次中位6.199/P9510.684/最大22.012ms。接受数不作无真值精度证据。
153份overlay inputGap中位4/P9520/最大57ms；最大片段track57/bounds4/present15/wait34，mapBuildMax3.750ms；全轮mapBuildMax7.250ms，未再采到上轮300ms窗口准备间隔，不宣称已修复旧间隔或鼠标绝对无卡顿。只分析/只读采样/记录，未改产品或刷新。活动峰仍910MiB，全应用600MiB目标未达。

证据out/cpu-memory-analysis/compact-marker-index-field-test-20261022/；native-census-20261022/compact-marker-field-stopped.json。

### 2026-10-22：内存阶段提交前验证

用户要求暂缓后续内存优化，先提交已完成的内存相关改动。范围包含紧凑运动源、释放build-only直方图、只读描述子映射、运行时索引压缩、SURF trace sign、编码候选缓存/有界解码、Paddle延迟加载、标记身份索引及配套诊断/回归工具；游戏帧率调查文档单独保留工作区，不并入此提交。

首次误用CMake全目标构建，连带外部WinUI工程使用VS MSBuild解析托管SDK，因找不到Microsoft.NET.Sdk失败；原始build.log保留。此次无.cs/XAML改动，随后显式构建CoreHost、相关工具与CTest列出的全部原生可执行目标成功。24/24 CTest通过（45.11秒），真实九目录24109点legacy/compact完整归一化与有序导出对照通过；两处门禁脚本PowerShell语法解析及暂存区diff --check通过。未修改既有断言，提交前仅清理新测试头文件多余EOF空行。验证证据out/cpu-memory-analysis/memory-commit-20261022/（native-targets、native-build、ctest、catalog-parity、staged-files）。本轮仅验证和提交，不刷新测试树、不push、不发布。
