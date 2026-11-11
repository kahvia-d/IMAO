# OCR接管调度、显示运动与完整缺口诊断

## 已授权范围

用户复核19:04:22录像后，授权顺序实施：优先优化OCR提交/结果消费，再加入只用于显示的受限运动估计，最后补齐可见缺口诊断。录像基线分析在主检出out/evidence/flicker-video-20261018-190422；本次验证在out/evidence/readout-motion-20261018。没有更新模型或修改托管/XAML。

## 1. 轻量服务与视觉工作分开排程

推理继续使用既有单工作线程，权威位置状态仍只在主定位线程修改，未引入第二个位置写入线程。OCR结果消费、门禁、接受后提前发布、下一请求提交均先于小地图SURF特征提取。接管时主循环目标间隔20ms，并仍只消费新截图；目标请求间隔100ms，识别运行时最小提交间隔同步为100ms。仍只有一个在途请求，不积压截图，不重复提交同一帧。

昂贵视觉工作在接管时独立按至少150ms间隔执行，不随每次轻量服务重复，持续为SURF交回提供真实图像证据。健康SURF原循环和1000ms待命不变。CoordinateHandoff的通用150ms建议仍保留；运行时接管请求由ReadoutWorkSchedule的100ms专用调度覆盖，替代前一轮ReadoutContinuity文档的接管请求设置。

这是协作排程，不是把所有视觉复核都移入新线程。个别视觉轮次仍可延迟下次消费，20/100/150ms均为调度目标，不是实机吞吐保证。开启旧定位诊断时会显式提前准备特征以保留原诊断功能。

coordinate-readout-latency逐结果记录captureToConsumeMs、submitToCompleteMs与completeToConsumeMs。它们与原captureSteadyMs/submitSteadyMs、readout-commit-to-layout共同区分采集、推理和消费等待。

## 2. 绘制线程的显示运动

ReadoutDisplayMotion从同区域、同UI代际、同投影比例的两次已接受地图坐标估算速度。新的有效fix先以其精确投影绘制；后续重绘根据真实截图时间最多向前估计200ms，以80ms时间常数收敛，显示位移封顶24屏幕像素。速度超过150地图单位/秒、观测间隔超过500ms、时间或帧号倒退、区域/代际/比例变化均清空运动历史。静止读数将速度收敛为零。

显示结果是统一OverlayScreenTransform平移，标记、自由点、路线和当前显示的交互几何共用；地图坐标Y向下，对X/Y都采用玩家显示位移的反方向平移。显示预测从不写回位置锁、coordinateTrust、routePlayer或任何证明时间。原始fix达到500ms仍拒绝绘制；新布局或截图不能续签期限。SURF恢复时清空显示状态，使用实测图像变换。

minimap-overlay-placement增加displayOffsetPx，用于判断实际帧间显示运动。既有投影断言和SURF交回断言保留，不通过改断言迁就新实现。

## 3. 完整显示资格缺口

保留原minimap-overlay-gap用于配准失败细节；新增minimap-visible-gap依据独立UI可见性、标记开关和焦点判断是否预期显示。即使发布端把布局minimapVisible或reliable清掉，也持续计时，直到实际恢复绘制（recovered）。真正离开小地图、关闭标记或失去可见上下文则单独记context-closed，不能当作恢复。

独立审阅发现旧大地图布局可能压制缺口计数；已改为完全由独立visibility快照判断地图上下文。缺口记录含durationMs、frames、eligible、sourceFresh、authorized、fixFrame与steadyMs。诊断测量的是绘制资格缺口，不是任意像素/目标过滤变化；实机仍需结合录像。

## 验证

新增ReadoutMotionTests覆盖视觉探测不饿死、轻量轮次跳过视觉、100ms请求设置、连续重绘运动、位移上限、区域/代际/比例/跳变重置、过期和缺口完整计时。CoordinateContinuityTests新增实际ImageAnchoredOverlay集成，验证同一fix在后续重绘仍运动且不能续签证明。原测试断言未改。

先见缺少实现的编译失败，再通过实现。另将显示前推临时归零进行对照，新增运动断言必须失败，恢复后重跑通过。原生Host、优化测试、异步识别运行时、交接/连续性/解码测试以证据目录日志为准。本次没有新实机测量，不宣称达到10Hz有效定位或闪烁归零。
