#pragma once
#include <atomic>
#include <cstdint>

// 工具总开关（用户 2026-09-21 定）：手柄 **LB + 按下 RS**、键盘 **F9**（可在设置里改）都能切换，
// 两种操作**既能开始探索，也能停止探索**。
//
// 关键取舍：它**不停止 CoreHost 进程**。输入监听必须一直活着，否则关掉之后玩家没法从游戏里再打开它。
// 暂停时只做两件事：
//   ① 叠加层不再绘制任何东西（标记、路线、大地图内容全部不画）；
//   ② 跳过定位/识别/视图解算这些"为游戏而做"的计算——这才是玩家关它的意义（省 CPU/GPU）。
// 恢复是瞬时的：不重新加载任何资源。
class RuntimeToolState {
public:
    static bool Enabled() { return enabled_.load(); }
    static std::uint64_t Revision() { return revision_.load(); }
    static void SetEnabled(bool value) {
        if (enabled_.exchange(value) != value) revision_.fetch_add(1);
    }
    static bool Toggle() {
        const bool next = !enabled_.load();
        SetEnabled(next);
        return next;
    }
    // 会话开始/结束时复位：新一次运行默认是启用状态。
    static void Reset() {
        SetEnabled(true);
    }
private:
    inline static std::atomic_bool enabled_{true};
    inline static std::atomic<std::uint64_t> revision_{1};
};
