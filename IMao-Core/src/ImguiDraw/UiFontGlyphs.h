#pragma once

// The common Chinese font range omits characters used by the actual controls
// (for example 辑). Keep explicit UI text in the high-resolution atlas as well
// as category display names loaded from map resources.
inline constexpr const char* IMaoUiGlyphs =
    "路线规划继续选点新建导航当前目标攻略已暂停结束指引跳过撤销实时开启关闭"
    "重新编辑退出移动地图矩形框选自由套索指定起点加入视野清空生成预览开始计算"
    "左摇杆选择确认返回游戏移动光标按住绘制松开提交取消鼠标拖动空白处点击切换"
    "追加手动打开前最后位置未知请完成定位工具栏正在根据调整接近不会自动标记"
    "保存设置未生效重试正在优化访问顺序预览已恢复档案目标变化本次圈选已取消"
    "大地图小追踪更新恢复等待界面匹配定位暂留不可用原目标冰青连续真实状态"
    "未能返回游戏松开全部按键后按重试或点击游戏窗口正在返回游戏请稍候路线操作已停止仅用于";
