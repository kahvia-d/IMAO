# 游戏内独立缩放 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox syntax.

**Goal:** 默认适配分辨率与 DPI，十项独立倍率设置，完成后刷新 out/map-test。
**Architecture:** 原生 OverlayUiScale.h 与托管 OverlayUiScale.cs 使用相同基础公式 max(dpi/96, min(width/1920,height/1080))，范围 1–4。倍率 50–200%，默认 100%，按可用空间约束窗口。配置由 RuntimeConfiguration 持久化并经 configure 同步原生。地图点位半径在绘制、合并、命中与快照统一。
**Tech Stack:** C++20 / ImGui / C# WinUI 3 / PowerShell。
**Spec:** 本文件下面的用户已确认设计。

## 已确认设计
捕获与叠加显示现有双栏下新增同主题双栏：左栏小地图状态条、大地图状态条、工具栏入口与窗口、攻略窗口、操作与错误提示；右栏大地图标记、小地图标记、手绘选项栏、手绘节点编号、小地图状态球。每项名称、倍率控件、百分比，100% 标注自动适配。不提供单项或全部重置，调回100%即恢复自动适配（按用户最新要求移除全部恢复自动）。文字/背景/点击区域同步、即时应用、持久化、旧配置默认100。特别验证重叠合并。

## Global Constraints
不修改 C:/Dapps/IMao；所有 build/test/publish 前 Enter-DevEnvironment；真实 WinUI publish 自包含；只用刷新脚本整树同步含 resources.pri；刷树前关闭测试客户端（用户已授权由代理核验路径后自行关闭）；逐文件 differing=0。保留已有测试断言。

## Review Focus
- 4K 100% 与 200% 不重复放大，1080p 高 DPI 不越界。
- 调整图标大小后合并阈值/点击/小地图快照保持一致。
- 旧配置缺字段、非法倍率、异步快速调节不丢最新值。
- 攻略窗/工具窗内部内容与外框同步，显示器变化重新布局。
- 手绘编号、提示文本、图标展开列表可读且可点击。

### Task 1: 配置与设置双栏
- [x] 在 RuntimeConfiguration、CoreHostService、SettingsPage 实现十项独立倍率并增加托管配置测试；先红后绿。
- [x] 创建 OverlayUiScale.cs：Automatic(double dpi,double width,double height), Effective(...,int percent)。
- [x] 属性与原生键：MinimapStatusScalePercent/minimapStatusScalePercent, MapStatusScalePercent/mapStatusScalePercent, ToolsScalePercent/toolsScalePercent, GuideScalePercent/guideScalePercent, HintScalePercent/hintScalePercent, MapMarkerScalePercent/mapMarkerScalePercent, MinimapMarkerScalePercent/minimapMarkerScalePercent, HandPaletteScalePercent/handPaletteScalePercent, HandNodeScalePercent/handNodeScalePercent, StatusBallScalePercent/statusBallScalePercent。

### Task 2: 原生缩放与交互
- [x] OverlayUiScale.h: enum class Part {MinimapStatus,MapStatus,Tools,Guide,Hint,MapMarker,MinimapMarker,HandPalette,HandNode,StatusBall,Count}; Automatic(dpi,width,height), Percent(Part), Apply(Part,int), Effective(Part,dpi,width,height)。线程安全，百分比50–200。
- [x] CoreHostMain 先验证所有倍率后应用；原生绘制使用该策略，字体/几何/命中同步。
- [x] 新增基础公式与 MarkerLayout 合并分离回归测试，先红后绿，不改既有断言。

### Task 3: WinUI 窗口缩放
- [x] MapToolsWindow / MarkerGuideWindow 与对应 controller 读取倍率，用统一公式按窗口 DPI 转换内容逻辑缩放；窗口矩形、遮挡、动画及焦点几何一致。
- [x] 验证打开窗口时配置变化和游戏窗口尺寸/DPI变化。

### Task 4: 集成与交付
- [x] 原生测试与构建、真实 WinUI 自包含 publish，界面视觉验证。
- [x] 独立审阅修复重要问题；确认客户端退出后 Refresh-MapTestBinaries，核验 includedFrameworks/resources.pri 与 differing=0。

## 验证结果（2026-11-11）
- 五套原生回归与完整托管回归通过；真实 WinUI 窗口测试 35 项通过。
- 1080p、1440p、4K 和 640×360 离线绘制检查通过，命中矩形未越界。
- 真实设置页验证十项默认 100%、工具栏调至 140% 后全部恢复自动；滑块使用 IMao 金色。
- 自包含 WinUI publish 后通过 Refresh-MapTestBinaries 刷新并重启测试树，396 个载荷文件 SHA256 对照 differing=0。
- 证据：out/overlay-sizing、out/overlay-ui-scale-native、out/overlay-scale-guide-full-green.log。未进行真实 4K 游戏会话人工验收。


