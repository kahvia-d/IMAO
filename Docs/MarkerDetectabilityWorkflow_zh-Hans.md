# 不可探测点位视频研究：新增视频与完成后清理

本文记录已完成研究的方法、真实脚本入口和缓存边界，供下次收到新攻略视频时复用。不是再次执行旧批次的指令，也不是要求永久保存视频。2026-11-01 用户明确允许：工作流留档后清理已完成任务的视频、音轨、截图及中间分析；保留可维护的最终成果、来源和身份依据。小体积结构化研究记录与原始转写继续保留。

## 当前成果与权威边界

- 2026-10-30 最终离线研究：12 视频、26 分 P、345 事件、378 收集奖励成员、378 去重身份，待审 0；其中 352 个单项确认、26 个集合成员。两次引导晶核属于任务前置，不计收集点位。
- 当前运行时收录权威是 `tools/MarkerDetectability/catalog.json`；生成文件是 `Assets/KuroMap/marker-detectability.json`。历史 `results/` 是当时研究快照，不可自动覆盖后续人工维护名录。
- `tools/MarkerDetectabilityV2/results/summary.json`、`positives.json`、`matching.json`、`unresolved.json`、`v1-diff.json` 与最终用户审核 JSON 保留研究结论和来源链。`scope` 中“未接入运行时”是当时离线导出器的措辞，当前接入状态参见 [名录维护](MarkerDetectabilityMaintenance.md)。
- 清单外保持 unknown；撤销一次身份确认不等于该点可探测。视频正式收集目标默认不可探测，是本批次用户指定的属性口径，记录在 `detectability-policy.json`；新视频是否适用必须依据当次用户要求，不能自行扩大口径。
- 378 是本轮完成数量，不是以后维护的固定断言。

## 环境恢复与费用

仓库根目录执行所有示例。任何 build/test/publish 前先执行：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
```

旧离线 Python 环境位于 `out/detectability-v2/runtime`，不是项目运行时依赖；可以重建，不能将 venv 当可搬运安装包。现存 `pyvenv.cfg` 标明 Python 3.12.7；以下版本从本机已安装 `.dist-info` 目录恢复，并非另有已验证的锁文件：

| 用途 | 本轮安装版本 |
| --- | --- |
| 解码、音频重采样 | av 19.0.2 |
| 图像、SIFT、矩阵 | opencv-python-headless 5.0.0.93、numpy 2.5.4 |
| 图片排版 | pillow 13.0.0 |
| HTTP | requests 2.34.2 |
| 本地 ASR | faster-whisper 1.2.1、ctranslate2 4.8.4 |
| ASR 相关运行库 | onnxruntime 1.31.1、tokenizers 0.23.3、huggingface-hub 2.3.0 |

复建前保留完整包版本清单，而不是仅复制此表。安装和模型恢复本身可能超过 100 MB，必须先估算并询问用户；不要把旧任务的下载授权沿用到新下载。本轮清理不需要下载。

历史转写采用 `Systran/faster-whisper-large-v3`，CUDA 为 `int8_float16`，CPU 为 `int8`。脚本尝试加载 CUDA 12.3 的系统 DLL；本机曾验证 RTX4060 可用。`transcribe.py --device cpu` 可显式选择 CPU；`retranscribe.py` 没有 `--device`，按脚本找到 CUDA 目录与否选择。CPU 版 torch 的检测不能证明 CTranslate2 不支持 CUDA。`scan_video.py` 使用 Windows `msyh.ttc` 字体；审核页 JS 测试需要 Node.js。

模型目录为 `out/detectability-v2/models/large-v3/`：必须一起保留 `model.bin`、`config.json`、`tokenizer.json`、`vocabulary.json`、`preprocessor_config.json` 和下载摘要。现存 `model.bin` 为 3,087,284,237 字节（约 2.88 GiB），见 `download-manifest.json`。本次保留完整模型和现有 runtime，作为未来视频共用工具，避免重新下载数 GB；模型不是逐视频临时媒体。

`download_model.py` 只实现 `model.bin` 分块下载、拼接与上游摘要核对，**不会补齐其余模型配置文件**；也会联网查询模型元信息，不是纯离线校验器。不能称“运行它即可恢复完整模型”。`chunks/`、`.partial`、`.assembling` 属于下载中间物，已验证完整模型存在时无需保留重复分块。

## 新视频先建独立批次，不能原样重跑旧驱动

目前没有统一的“导入任意新视频”CLI。多数脚本的 `ROOT` 来自脚本位置，并固定读取 `out/detectability-v2/manifest.json` 与 `tools/MarkerDetectabilityV2/timelines/`。单改当前工作目录不能隔离它们。

尤其 `prepare_media.py` 无 CLI 参数，会读取 `out/detectability-collection/inventory.json`，重建旧 manifest，并为旧黑海岸 BV 硬编码专用视频路径。`normalize_routes.py`、`route_huanglong_late_*.py`、Spike 区域脚本及 `out/` 的一次性批处理也带历史范围；不可拿来覆盖本轮最终时间轴或盲目用于新视频。

新批次先使用独立研究 checkout/目录，保留脚本相对仓库根的结构与所需地图资源；或先对脚本做经过审查的批次路径参数化。现有历史时间轴、overview 和 results 不放进新批次的活动输入目录。将新数据写入哪个位置、如何合并，是新任务的显式实施工作；本文不假装这些参数已经存在。

不改代码的隔离布局可在一个全新研究目录中按下表建立；复制仅限列出的输入，不能复制整棵旧 `out/` 作为新批次。新目录名由当次任务指定。将工具代码复制到新目录后，脚本 `parents[2]` 即指向该目录；从该目录运行下文命令。

| 新研究根下固定路径 | 放入内容与注意事项 |
| --- | --- |
| `tools/MarkerDetectabilityV2/` | 复制 `.py` 与 review_template.html；手工创建空 timelines、first-pass、results 目录，不复制旧时间轴/overview/审核结论 |
| `tools/MarkerDetectabilitySpike/` | 需要下载/攻略辅助时复制工具源码；不导入旧 collection-results 作为首轮候选 |
| `Assets/KuroMap/states/` | 当前真实点位快照；保留资源版本，后续身份确认依赖它 |
| `Assets/FeaturesDatas/KuroTilePacks/` | 当前所需瓦片包及 manifest；包含真实所需分层资源，不能用地表替换 |
| `out/detectability-collection/inventory.json` | 新 BV 的目录清单（数组，每项 bvid/title/pages；page 项含 page/cid/duration/part），供 collection_utils.download 读取 |
| `out/detectability-collection/<BV>/pNN/video.mp4` | 新批次标准视频路径；`route_follow.py` **直接固定读取此路径**，不读 manifest.videoPath |
| `out/detectability-v2/manifest.json` | 只含新批次全部分 P 的 manifest 数组，字段见下段 |
| `out/detectability-v2/<BV>/pNN/` | audio.m4a、media.json、transcript.raw.json/txt、visual 等新批次文件 |
| `out/detectability-v2/models/large-v3/` | 可复制已下载模型，或在明确核对目标后建立指向旧模型的目录联接；联接只用于研究工具，不用于发布包，删除时不得递归跟随目标 |

Python 可直接调用原研究树保留 runtime 的解释器绝对路径；解释器位置不会改变复制脚本的 ROOT。跨目录复制模型无需下载，但会占用额外磁盘。原开发树始终保留最终历史研究与名录。新批次交付后只将审核通过的增量和来源记录带回，不整树覆盖维护名录。

为每个分 P 建 manifest 项，字段为 `bvid`、整数 `part`、字符串 `cid`、`duration`、`title`、`partTitle`、仓库相对 `videoPath`。保留页面目录、来源标题、来源身份及媒体 SHA256/字节数。来源链接可由 `https://www.bilibili.com/video/<bvid>/?p=<part>` 恢复，不需要保存会过期的签名 CDN URL。合集和分 P 清单必须人工核对，前言也纳入覆盖范围。

新文件从空模板建立，不把旧记录的 reviewed 状态复制过去。例如 `tools/MarkerDetectabilityV2/timelines/BV_NEW-p01.json` 初始内容：

```json
{
  "bvid": "BV_NEW", "part": 1, "duration": 120.0,
  "reviewStatus": "pending", "audioReviewStatus": "pending",
  "progressBarReviewStatus": "pending",
  "events": [], "nonEventIntervals": [], "reviewedEvidence": []
}
```

120 秒仅示例，换为实际容器时长。审阅时加入 `eventId=BV_NEW:p1:e001`、ordinal/start/end、evidenceFrames、transcriptRefs 及 rewards；每个 reward 的 member 从 1 连续编号，triggerSeconds/appearanceSeconds/collectionSeconds 可为 null，identityStatus=pending、detectability=unknown、firstPassStatus=pending。未覆盖区间先保持错误，不凭空填“无事件”。transcriptRefs 精确复制原始 segment 的 start/end/text。`media.json` 以 prepare_media.prepare(item) 实际输出为模板，video/audio 各含 duration、streams、path、sha256、bytes；不能手填虚假摘要。

首轮完成后才建立 `overview-review.json`：顶层 status=reviewed、parts 数组恰好包含 manifest 每个 `(bvid,part)` 一次，各项需要 status=reviewed、overviewPresence、非空 analysis 与真实 evidence 路径；未审前不要写 reviewed。完整证据格式和 group 字段分别以 contract.py、group_identity.py 为准，不能通过把字段补齐来代替事实审核。

下载前估算总量，超过约 100 MB 先询问用户；本地等价物先核对摘要再复用。Spike `collection_utils.download()` 仅凭目标与 `media.json` 存在便复用，不能据此声称已重验摘要。视频常是 DASH 无声流，需要另取音轨；音轨时长、起始时间戳和画面对齐不能省略。

## 顺序与人工门禁

1. **先媒体和来源。** 整理新 manifest；下载或复用视频，准备独立音轨与 `media.json`。可复用 `prepare_media.prepare(item)` 的函数逻辑，但先检查输入路径，不能调用旧 `main()` 重建历史批次。缓存要核对音频/视频 SHA256、大小、解码流、时长与起始时间。网络失败显式记录，不打印签名 URL。
2. **再独立时间轴。** 运行本地 ASR、按画面进度条与音频文本检查所有分 P。按视频出现顺序编号事件；同一事件的多箱是多个 reward member，不是按旧编号强拆事件。记录触发、出现、领取秒数，未见到时保留 null。事件区间与有理由的 `nonEventIntervals` 必须覆盖整片。
3. **所有分 P 的时间轴审核结束，才做定位。** `reviewStatus`、`audioReviewStatus`、`progressBarReviewStatus` 均 reviewed，且 `validate_timeline()` 无错，`can_start_matching()` 才开放。自动转写、抽帧页生成都不能代替人工审核；使用 local-ASR-text-and-visual 时写明没有直接回听。
4. **独立首轮身份核对。** 用小地图 SIFT/RANSAC、北向地形、真实信标/码头/副本与连续路线缩小范围，再查该候选自己的库街区攻略。位置、视频机制/场景、攻略三类支持分别记录；候选、诊断失败、排除项分别记录。失败或歧义保留 reviewed-unresolved，不按最近坐标猜 ID。
5. **完整首轮结束才看总览补漏与旧结果。** 每个奖励都必须有 reasoned matchingReview；确认与未决都算完成首轮，pending 不算。冻结首轮快照，再做全分 P overview，记录 present/not-found/uncertain 及依据。旧版差异仅在此门禁后查看，避免旧编号、旧候选污染新定位。
6. **逐项/集合复核并保留更正链。** 允许用户确认补足身份，明确依据与证据限制；更正保留原决定和撤销原因，不静默覆盖。没有攻略图片必须明确写出。最后生成离线结果，审核名录增删，再运行生成器。

时间轴结构和门禁真实定义在 `contract.py`；原始 ASR 不改写，文本校正另记原文、建议文字和画面/上下文依据。`validate_dataset.py` 验证结构、文件存在、原始 ASR 引用和容器时间，不验证事实判断，也不证明已直接回听。

## 已有可运行入口及限制

下面命令只在**新批次已隔离、manifest/媒体/时间轴配置已准备好**时按阶段使用。`BV_NEW` 和秒数是需替换的示例值，不是新增脚本参数。若保留旧 venv，可将 `$researchPython` 指向它；否则指向复建环境。

```powershell
$researchPython = '.\out\detectability-v2\runtime\Scripts\python.exe'
& $researchPython tools/MarkerDetectabilityV2/transcribe.py --device cpu --bvid BV_NEW --part 1
& $researchPython tools/MarkerDetectabilityV2/scan_video.py BV_NEW --part 1 --start 0 --end 120 --step 5
& $researchPython tools/MarkerDetectabilityV2/retranscribe.py BV_NEW --part 1 --start 30 --end 45
& $researchPython tools/MarkerDetectabilityV2/build_review.py
```

`transcribe.py` 可选 `--device cuda`；缓存匹配 audioSha256、modelSha256 和 config 才复用。`scan_video.py` 生成发现用画面与原宽进度条，不保证无遗漏。`retranscribe.py` 是 beam10、无 VAD 局部复识别，不覆盖原始转写。`build_review.py` 只有 `--output`，没有 `--root` CLI；默认产物是 `out/detectability-v2/review/index.html` 和 `build-report.json`。必须检查 `pathIssues`，还要实际验收画面、跳转和音视频播放；Node DOM 测试不能冒充真实浏览器验收。

```powershell
& $researchPython tools/MarkerDetectabilityV2/localize.py --inventory
& $researchPython tools/MarkerDetectabilityV2/localize.py --config out/detectability-v2/localize-config.json
& $researchPython tools/MarkerDetectabilityV2/route_follow.py BV_NEW --part 1 --start 30 --end 80 --step 1
```

`localize-config.json` 需另行编写。实际配置结构见 `localize.py` 文件头：`references`、`frames`、`output`；ROI 是原图像素，记录 `segmentId`、`cutBefore`，输出必须在 `out/detectability-v2` 内。瓦片 reference 需要明确 manifest、bounds、scale；专用分层图 reference 需要 image、pixelToGame、region、scene、state、layer、provenance。地表瓦片不能改标签冒充地下图。`localize.py` 在读真实帧前检查全量时间轴门禁。

`route_follow.py` 同样受全量时间轴门禁限制，记录相对地形运动，不能给出绝对点位身份；跳剪、缩放、失败连边、传送必须人工检查，不能跨断链累计路线。

人工路线报告经审阅后可运行：

```powershell
& $researchPython tools/MarkerDetectabilityV2/route_import.py out/detectability-v2/new-reviewed-routes.json
```

报告需要顶层 `entries`；逐行 `status=reviewed-route-and-guide` 才导入，并检查 evidence 文件与 `apply_review()` 定义的字段。该命令会写活动时间轴，不是只读检查。不要把 SIFT 输出直接包装成“人工审核通过”。集合身份由 `group_identity.py` 的结构与校验维护，没有独立“自动认箱”CLI。

媒体和图片仍完整时才运行整库审计与离线导出：

```powershell
& $researchPython tools/MarkerDetectabilityV2/validate_dataset.py --expected-parts 1 --report out/detectability-v2/new-audit.json
& $researchPython tools/MarkerDetectabilityV2/delivery.py
```

将 `--expected-parts 1` 改为实际分 P 数（默认是历史 26）。validator 支持 `--root`、`--manifest`、`--timelines`、`--alignment-tolerance`；但每 P 的 media/transcript 仍按 root 下固定 `out/detectability-v2/<BV>/pNN/` 查找。`--report` 不能覆盖已有文件，要换新路径。`delivery.py` 只有 `--root`，固定读取 overview-review.json，并验证图片/攻略/集合等引用真实存在后才覆写 results。上述入口不允许跳过缺失媒体以获得“全绿”。

## 常见误判与必须保留的理由

| 风险 | 正确处理 |
| --- | --- |
| 视窗边缘箭头被当玩家坐标 | 检查地图自身视窗边界及前后帧；拖图时箭头可钳在边缘，只表示离屏方向。不能只检查箭头是否位于整个视频画面内部。 |
| 集合 ID 已知、逐箱顺序未知 | `identityGroups` 用 confirmed-set、members、points 与各自攻略依据；导出 `member=null`，保留 groupId 和完整 members；不按数组顺序强配。 |
| 终点小地图失败 | 用已确认锚点、连续路线、地形与本点攻略核对；不能把片尾玩家位置反推所有箱子。 |
| 最近坐标/相同箱色被当唯一身份 | 坐标仅筛候选；比较机制、建筑、层级及路线。冰洞奖励品质可随次序改变，箱色不能单独否定。 |
| 摩托挑战起终点、道具界面变暗 | 核实实际回起点领奖；界面变暗不能单独推断剪辑。 |
| 共用攻略图或相近 ID | 每个 ID 保留自己的攻略 JSON/来源；共享画面只能记录关系，不能制造逐成员身份。 |
| 候选图与终点冲突 | 单列 exclusionEvidence，不得同时当 support；撤销排除须写新依据和旧理由。 |
| 攻略缺图片或用户排除法确认 | 如实记录缺失和用户依据，不声称做过图片匹配，不编造攻略链接。 |

## 从离线结论维护名录

先审查新确认身份与现有名录的增量，保留字符串 stateId/pointId、视频 BV/分 P/事件及成员或集合、reviewBasis、notes。事件秒数和详细依据可由保留时间轴与 results/matching.json 追溯；名录不单独包含全部秒数。不要仅保留生成 ID 列表而丢掉研究来源。

```powershell
. .\scripts\Enter-DevEnvironment.ps1
python tools/MarkerDetectability/generate.py
python tools/MarkerDetectability/generate.py --check
python -m unittest discover -s tools/MarkerDetectability
```

生成器只读维护名录与 `Assets/KuroMap/states/state-*.json`，不依赖下载媒体。检查失效 ID 时核对上游迁移/合并，由用户需求决定改配或撤销，不能按最近坐标自动替换。改了 XAML 或托管代码另须真实 WinUI 构建；这不是研究缓存清理的一部分。

## 完成后保留清单

以下按可移植相对路径保留，不依赖当前机器的绝对路径。`out/` 被忽略，留在这里不会自动进入 git；清理工具需明确保留白名单并核对副本，而不是整目录删除。

| 必须保留 | 作用 |
| --- | --- |
| `tools/MarkerDetectability/` 全部源文件、catalog；`Assets/KuroMap/marker-detectability.json` 与对应 state 数据 | 可维护名录、生成器、当前运行数据和检查 |
| `tools/MarkerDetectabilityV2/` 全部版本控制内容，包括 timelines、first-pass、results、review JSON、policy、模板、脚本和测试 | 最终结构化审核、来源秒数、用户确认、集合关系与修订理由 |
| `tools/MarkerDetectabilitySpike/` 全部版本控制内容 | 旧来源目录、v1 对照、早期结构化结果和工具 |
| `out/detectability-v2/manifest.json`、`media-preparation.json`；每分 P `media.json`、`transcript.raw.json`、`transcript.txt`、`transcript.repeat-*.json` | 来源/cid/媒体摘要、原始与重复转写；不保留音轨也可追溯当时引用 |
| `out/detectability-collection/inventory.json`、`media-manifest.json`、各分 P `media.json`；所有攻略 `guide-*.json` 及 guide 索引/下载状态 JSON | 可恢复来源和攻略文本、图片 URL、ID 与摘要，不要求保留图片 |
| 三个 detectability 输出树中研究自产的 `.json`、`.jsonl`、`.txt`、`.csv`、`.srt`、`.vtt`、`.md`、`.py`、`.ps1` 配置和记录（runtime、models 单独整套保留） | 保存路线/匹配数值、人工审阅、参数、一次性批处理以解释或复用方法；不要仅按文件名“final”筛选 |
| 清理前的环境版本清单、媒体删除清单与清理前 audit/build-report | 明确曾保留什么、删了什么、历史验证发生于何时；报告不等于清理后复验 |

没有必要把所有中间分析继续执行；小 JSON 中的候选、失败诊断和原决定仍有解释价值，保留成本小。可重新下载的攻略 JSON 也可能日后改版，优先保留当时版本。若要归档迁移，保持相对目录结构与来源定位关系；改目录会让原脚本路径失效，需要显式映射。

## 可清理范围与清理后的验证含义

仅限本研究完成后的以下目录内容，不触及其他输出、桥接工作或玩家安装：

- `out/detectability-collection/` 的视频、图片、抽帧/拼图、review HTML、`.npz/.npy` 图像索引与其他可重建数值缓存；先保留上一节结构化文件。
- `out/detectability-spike-blackshores/` 的 `video-480p.mp4`、帧图、攻略图片、地图、overlay/matches/comparison 图片与 review HTML；先保留 JSON/脚本等小记录。
- `out/detectability-v2/` 的每分 P `audio.m4a`、其他媒体、visual 图片、review 页及路线/定位/校准图片，特征缓存 `.pickle/.pkl/.npz/.npy`；保留 JSONL 及其他小结构化记录。`runtime/`、`models/` 的整个子树排除在媒体/缓存删除范围之外，避免误删它们内部使用的格式相同文件。
- 本次 **保留** `out/detectability-v2/models/` 和 `runtime/`；日后若再决定删除，必须先明确接受重建成本和下载审批要求。

清理前核对所有目标绝对路径、目录联接与保留白名单；不能将上述描述直接翻译成对整个 `out/` 的递归删除。不能触及 `C:\Dapps\IMao`。本文件仅记录规则，不执行删除。

媒体删除后，时间轴和研究 JSON 里的原图片/视频 locator 是历史来源说明，实际文件链接会失效：

- `validate_dataset.py` 会报 missing-media / missing-evidence；`delivery.py` 的 audit_references 与 overview 检查会拒绝导出；Spike `validate_collection.py` 还要求旧媒体 SHA256 和图片可读。这些失败应如实报告，不能删断言、伪造空文件或修改证据路径来通过。
- `build_review.py` 能生成页面和 pathIssues，但重建页面不是补回图片、音轨或视频；不能再宣称审核页“零缺失”。删除 review HTML 后，现有 `test_review.py` 的 `test_javascript_all_events_and_media_sync` 会因依赖历史生成页而失败。将它改成临时自包含 fixture 是未来独立测试维护工作，本次不修改测试。
- `test_validate_dataset.py` 已用临时生成的媒体 fixture，`test_audio.py`、几何/合同/集合等测试也有独立样本；它们通过只证明脚本行为，不能证明被删除历史证据仍存在。整套 V2 测试不能在历史页面缺失后直接宣称全部通过。
- 维护名录生成器及其测试仍可在不含视频的 checkout 中运行。保留的清理前 valid=true 报告只证明当时检查；清理后报告需明确“最终成果保留，历史媒体审计不可重跑/预期缺失”。

清理后可单独运行不依赖历史素材的测试模块：`test_audio.py`、`test_contract.py`、`test_compare_v1.py`、`test_delivery.py`、`test_group_identity.py`、`test_localize.py`、`test_route_follow.py`、`test_route_import.py`、`test_validate_dataset.py`。它们使用内存或临时构造媒体/图片；依赖仍需安装。示例（从仓库根，先加载开发环境）：

```powershell
. .\scripts\Enter-DevEnvironment.ps1
Push-Location tools/MarkerDetectabilityV2
try {
    & ..\..\out\detectability-v2\runtime\Scripts\python.exe -m unittest test_audio test_contract test_compare_v1 test_delivery test_group_identity test_localize test_route_follow test_route_import test_validate_dataset
} finally {
    Pop-Location
}
```

`test_review.py` 前三项也使用临时 fixture，但整个模块包含上述历史页依赖测试，不能将它混入该子集后声称清理后全套通过。本次不修改或放宽任何测试断言；旧批次的全媒体复验在素材清理后停止适用，需要补回相应素材才能重跑。

以后需要重审某个身份时，先从保留 BV/分 P/事件秒数和攻略 JSON 恢复目标范围，再按下载费用规则取得所需证据；不必为新增视频重新下载已完成的全部 26 分 P。
