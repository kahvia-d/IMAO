# OCR显示完全关闭预测与平滑的对照（2026-10-18）

用户明确要求先做一次完全关闭的测试并刷新测试树。本次关闭的是OCR接管时的显示运动预测与平滑；YOLO权重/预处理、识别门禁、可信位置校验及SURF接管切换不变。

## 运行方式

ReadoutDisplayMode增加Direct和Animated两种模式，kRuntimeReadoutDisplayMode明确设为Direct。真实绘制器构造minimapMotion时传入该运行时模式；ReadoutDisplayMotion保留作为上一轮动画对照，不进入Direct运行路径。

Direct在原有截图、焦点、场景/代际、几何及500ms原始fix时效检查通过后返回单位变换，完全保留发布器基于最新接受坐标的投影。没有新有效读数时位置不随等待改变；新读数到来立即更新。标记、路线和交互继续共用同一个变换。

App主循环在OCR接管Direct模式下也跳过显示预测和80ms坐标收敛，附近点位按已接受坐标选择。用于识别校验和影子测量的坐标趋势逻辑不属于显示运动，保留。健康SURF的实测图像跟随不受本实验开关影响。

minimap-overlay-placement增加displayMode=direct，displayOffsetPx应始终为0,0；minimap-render-position应为mode=readout-direct。读数过期仍隐藏，不为了对照延长可信期限。

## 验证

新增TestDirectReadoutDisplay覆盖空白地形、快速移动、反向、静止、新读数和重复渲染；要求屏幕变换严格为单位变换并保留原始坐标/时间，500ms到期仍拒绝。direct-control显式启用Animated，必须触发同一零补偿断言失败。原有动画回归保留并继续验证Animated路径，未修改既有断言。

原生宿主、交接、解码、连续性、运动、优化及异步识别运行时的实际构建/测试证据在主检出out/evidence/readout-direct-20261018。整树载荷、resources.pri、自包含运行时和逐文件核对记录在out/evidence/maptest-readout-direct-20261018。没有修改玩家安装目录。

这是实机对照入口，未宣称关闭后地图漂移一定消失。预期能观察到OCR离散更新和正常采集/识别延迟；实际贴合误差要结合下一次录像判断。
