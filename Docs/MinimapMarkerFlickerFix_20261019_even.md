# even 小地图标记批量读取修复（2026-10-19）

分支：`codex/even-map-marker-flicker`；测试版：`2026.10.19.103`。
用户已授权本地修复和打包，交给 even 复测；本次不进行线上发布。

## 问题与改动

前置排查见 [`MinimapMarkerFlicker_20261018_even.md`](MinimapMarkerFlicker_20261018_even.md)。
even 已确认 `2026.10.14.102` 解决大地图闪烁，但小地图仍闪烁。
小地图仍逐类别调用旧完成 ID 读口，附近距离筛选之前反复扫描整份进度。

小地图现复用大地图的 `ReadPointFilterCompletions`，每轮拥有一份完成状态值快照，
通过集合查询设置点位的 `isSaved`。不缓存跨轮完成状态，因此撤销、云端更新和每日重置
仍在下一轮读取生效。永久与每日账本属于不同账号时拒绝本轮点集；快照不把旧账号点集
重新标成新账号。账号绑定与点集共同受 `markerMutex` 保护。

为了直接测试生产代码，把小地图点集生产、快照和相关状态从绘制文件搬到
`DrawItemOnMinMapFilter.cpp`，绘制与交互方法仍在原文件。
保留原有 120 / 155 单位进出场迟滞、圆形裁剪、投影、完成显示与楼层规则。
源帧有效期、定位信任和 150ms 画面附着复用窗口均未放宽。

## 新诊断

- `minimap-overlay-cycle`：每两秒抽样一个生产周期，分别记录 `positionMs`、`filterMs`、
  `publishMs`、`cycleMs`、`captureAgeAtStartMs`、`publishedAgeMs`、`maximumAgeMs`。
  `positionKnown` 和 `filterRan` 区分定位失败与未执行过滤；`frame` / `publishedFrame`
  区分新发布与沿用原始可信帧。
- `overlay-motion` 新增 `minimapRequestedFrames`，及互斥的六类结果：
  `minimapAttachedFrames`、`minimapHeldFrames`、`minimapStaleFrames`、
  `minimapContextRejectedFrames`、`minimapVisibilityRejectedFrames`、`minimapAttachRejectedFrames`。
  在发布侧大小地图可见性互斥的前提下，六项之和等于 requested。
  这些是渲染迭代计数，不是闪烁次数。
- 保留已有 `minimap-markers-cleared`、`minimap-overlay-drop` 与位置诊断，
  用来继续区分定位、源帧过期和地形画面附着问题；普通日志即可，无需打开截图诊断。

## 回归和对照

新增 `IMaoMinimapFilterTests` 链接实际小地图筛选、快照、屏幕投影和距离计算代码，
仅用测试边界提供所选数据与隔离的真实进度账本，诊断输出为空操作。
没有在生产类添加测试专用方法。

24 类、12000 条进度，只有一个附近点的合成数据：

- 原始生产筛选：旧读取 113.789ms，小地图过滤 114.574ms，比例 1.00689，
  “至少消除四分之三重复读取成本”的回归按预期失败。
- 批量读取后：旧读取 114.712ms，小地图过滤 4.460ms，比例 0.03888，约 25.7 倍。
- 通过完成状态、每日重置、撤销、120 / 155 单位迟滞、中心投影、圆形裁剪、空选择，
  及切换账号后旧帧拒绝 / 两个账本切换不同步时拒绝 / 新账号不继承旧进度检查。
- 既有 `IMaoMarkerTests` 继续覆盖完成读取的分类、本地覆盖、分层状态、并发账号切换与性能。
- 独立静态复核未发现需修正的问题；复核本身不作为运行测试证据。
- 完整原生构建通过；CTest 8/8 通过（优化、资源快照、标记、小地图筛选、分层地图、
  路线规划、手绘路线、多场景视口），额外路线服务测试 `failures=0`。
  编译仍有既有数值转换和 zlib / CRT 链接警告，没有编译或链接错误。

日志保存在本地 `out/evidence/even-minimap-marker-flicker-20261019/`。
上述耗时属于本地合成数据，不能冒充 even 本机耗时或实际闪烁验收。

## 交付与实机验收

自包含测试包由标准 `New-ProgramReleasePackage.ps1` 整树组装，包含 PRI、原生宿主、
所需地图和运行库，附独立目录运行说明；不覆盖玩家安装或已有开发测试树。
完整构建、测试及包核验结果以本次本地证据为准。

even 可正常游玩；可选在大世界关闭大地图后站立约 30 秒、跑动约 60 秒，
下层金库重复并返回大世界。若仍闪烁，记录时间并提供完整日志目录。
验收同时检查过滤耗时、六类渲染结果和玩家肉眼反馈，不把代码修复等同于实机问题已全部解决。
