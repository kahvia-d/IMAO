# 2026-09-06 地图识别根因审计

诊断依据：`%LOCALAPPDATA%/IMao-WinUI/Diagnostics/20260906-210857/events.log`、同目录截图、当天结构化日志。回放素材保存在 `Tests/VisualLocalization/20260906-regression/`。

## 结论与对照实验

### 大地图：重叠特征包破坏最近邻距离比检验

最新日志已确认 `state=BigMap`、`mapControlsVisible=1`。定位失败发生在匹配阶段：1576 个原始截图特征几乎没有通过描述子过滤，尚未进入有效几何拟合。继续调开图识别、UI 坐标或 RANSAC 阈值不能解决它。

Dreamzhou 与 DreamzhouWest 的瓦片范围重叠。原始包按行直接合并，5973 条坐标和描述子完全一致的记录成为不同的训练行。查询的第一、第二近邻因此可能是同一地标的两份副本，`d1 < 0.62*d2` 必然失败。

使用同一张 `32_state-change-full.png`、同一 SURF 参数与距离阈值的对照结果：

| 候选库 | 原始行数 | 距离相等的近邻对（距离小于0.5） | 通过匹配 | 几何内点 |
|---|---:|---:|---:|---:|
| Dreamzhou 单包 | 14961 | 2 | 48 | 44 |
| DreamzhouWest 单包 | 6543 | 1 | 44 | 43 |
| 原合并库（含未启用 BlackShores，诊断对照） | 277863 | 103 | 0 | 0 |
| 相同合并库，仅去重 | 271890 | 2 | 32 | 32 |

两个单包独立得到的视口中心约为 `(-6323.58, 1152.70)`，与去重结果一致。该数值仅用于回归对照，未写入运行时定位逻辑。FLANN 是近似检索，重复运行的匹配数量可能略有差异。

修复在构造同场景候选描述子集合时去除完全重复的“地图坐标+描述子”。相同外观出现在不同坐标时仍保留，继续作为真正的歧义候选。原 IMF/IMX 行编号不变，避免破坏分片引用。小地图几何验证和投票也使用同一去重规则。

### 小地图：生成器按大网格截断，丢掉局部可见地形

生成器原来按 420×420 地图像素分组，每组只保留响应最强的160条特征。小地图可见地形只是其中一小部分；同格内建筑等高响应纹理会挤掉附近河岸、道路等较弱但必要的特征。

使用清单固定版本 `E62CEAC5F80745288BF76C7AD5F731C3` 的原始 `8_-8_1.png`，下载后核对 SHA-256 与原清单一致。最新第一张小地图归一化后有84个特征：旧 Dreamzhou/DreamzhouWest 包的相似变换都只有2个内点，模型不成立；原始瓦片未截断特征得到14个内点、尺度1.067、近似零旋转。

改为保留所有通过原 SURF 检测阈值的观测，维持原有检测器、匹配阈值、比例约束和几何验收条件。对两个已验证的 World 包全部58张瓦片重建，而非补录当前位置：

| 包 | 原保留数 | 重建保留数 |
|---|---:|---:|
| Dreamzhou | 14961 | 63131 |
| DreamzhouWest | 6543 | 30520 |

IMF 转换执行逐字段往返校验，所有 IMX 分片重新生成并验证词表和源文件哈希。BlackShores 及其他未通过实景验证的包继续保持原有未启用状态。

### 候选筛选和测试：强几何证据被降级，离线发布条件不一致

扩展包回退搜索的 `lowDensityButStable` 分支原来无条件把满足条件的候选设为 Marginal，包括已经达到 Strong 的候选。同等级排序又可能让数量较多的固定比例平移投票取代已拟合几何的候选。现在只提升原本 Rejected 的稳定几何候选，并在同质量等级下优先保留拟合几何。

离线 VisualRegression 原来没有调用实际程序使用的 `HasReacquisitionSupport`，可能把运行时拒绝的纯平移候选算作发布成功；现已对齐。历史锚点校验及与索引中同源截图自匹配，仅证明资源坐标/兼容性，不能替代陌生实景的全局定位测试。

## 可复现验证

```powershell
.\x64\Release\IMaoOptimizationTests.exe
.\x64\Release\IMaoVisualRegression.exe $PWD .\Tests\VisualLocalization\20260906-regression.json .\out\recognition-audit\idle-minimap.json
.\x64\Release\IMaoVisualRegression.exe $PWD .\Tests\VisualLocalization\20260906-viewport.json .\out\recognition-audit\final-viewport.json
.\x64\Release\IMao-CoreHost.exe --check-resources .\Assets
```

小地图清单包含最新10张、先前会话13张、两组历史锚点及确认帧、空白/噪声反例。真实移动截图没有独立测量坐标，不把新算法自身输出充作绝对真值。大地图清单包含实际定位器、分辨率/亮度变体及反例；中心对照来自单包独立匹配，检验的是合并与变换一致性。

最终运行结果见本文件后续记录。特征完整保留会增加资源体积和部分回退搜索成本，需要单独观察延迟；不能仅凭“特征资源可加载”宣布所有地图、楼层均已正确识别。

## 扩展回放的边界

- 早先会话 `20260906-153705/24_minimap-feature-source.png` 实际内容是“玄方城 / 探索度100%”大地图标题，日志记录随后才确认 Gameplay→BigMap；该样本列为必须拒绝。
- 同会话 `21_minimap-feature-source.png` 仍是真实小地图，但有明显覆盖干扰。截图本身无法提供足够稳定匹配，新版本仍拒绝，不能宣称该类遮挡已修复，也不应降低几何门槛强行给坐标。
- 距离当前游戏实景最新采样时间为2026-09-06晚；2026-09-07的工作是离线重放与构建，未进行新的游戏内实测。
- 已有可复核的 Marginal 相似变换无需再进行整包搜索：直接按原有附近复核和连续帧确认规则处理。没有可靠几何候选时仍保留全局恢复。

## 最终验证结果（2026-09-07）

- Release 核心和回放工具编译成功；`IMaoOptimizationTests` 通过。
- 两个重建特征包校验通过；最终发布目录 `x64/Release/Assets` 的 `resourcesReady / visualReady / viewportReady` 全部为 true。
- 大地图：8/8 回放通过。原图205个几何内点；720p、900p、1080p、1440p、1800p分辨率及亮度变体与单包中心对照偏差均小于0.5地图像素；空白、噪声拒绝。记录：`out/recognition-audit/final-viewport.json`。
- 小地图：29个样本均有结果，无缺失；24 Strong、1 Marginal、4 Rejected。最新会话10/10均通过附近复核。4个拒绝分别为明显覆盖干扰的真实小地图、大地图标题、空白和噪声。记录：`out/recognition-audit/idle-minimap.json`。
- 带标注及必须拒绝样本未发生误接受，acceptedPrecision=1.0。未标注的移动截图不能据此宣称绝对坐标准确率100%。
- 编译结束后单独复测：acceptedGlobalP95=254.4495ms，localTrackingP95=33.6601ms。前者略超项目250ms门槛，故工具退出码3、`acceptancePassed=false`，如实保留。部分无有效几何的整包/旋转恢复仍明显较慢，是剩余性能问题。
- 版本已同步到 `x64/Release`，可重新启动工具使用。没有进行新的游戏内实测；不能宣称所有区域、楼层和遮挡状态均已验证。
