#include "RuntimeStatusBar.h"

#include "../../Runtime/RuntimeStatus.h"
#include "../ImGuiOverWindows.h"

#include <string>

namespace {
constexpr ImGuiWindowFlags kStatusFlags = ImGuiWindowFlags_NoDecoration |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

const char* GameStateText(const std::string& state) {
    if (state == "gameplay") return "小地图";
    if (state == "bigMap") return "大地图";
    if (state == "enteringBigMap") return "正在打开大地图";
    if (state == "leavingBigMap") return "正在返回游戏";
    return "等待游戏界面";
}

std::string LocalizationText(const std::string& state, const std::string& quality) {
    if (state == "tracking") return quality.empty() ? "小地图追踪" : "小地图追踪（" + quality + "）";
    if (state == "mapTracking") return "大地图定位完成";
    if (state == "mapLocating") return "大地图定位中";
    if (state == "stale") return "使用上次定位";
    if (state == "recovering") return "正在恢复定位";
    if (state == "waiting") return "等待定位";
    return "等待定位";
}

ImVec4 StatusColor(const std::string& state) {
    if (state == "faulted") return ImVec4(0.95f, 0.32f, 0.32f, 1.0f);
    if (state == "recovering" || state == "mapLocating" || state == "stale") return ImVec4(0.95f, 0.72f, 0.24f, 1.0f);
    return ImVec4(0.25f, 0.86f, 0.48f, 1.0f);
}
}

void RuntimeStatusBar::Draw(HWND gameWindow) {
    const auto status = RuntimeStatus::Snapshot();
    if (!status.statusBarEnabled || !status.gameFocused || status.coreState != "running") return;
    const HWND foreground = GetForegroundWindow();
    if (foreground != gameWindow && foreground != ImGuiOverWindows::overWindowsHwnd) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float kWidth = 430.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
        viewport->WorkPos.y + 18.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.78f);
    if (!ImGui::Begin("IMao Runtime Status", nullptr, kStatusFlags)) {
        ImGui::End();
        return;
    }

    const std::string lineOne = std::string("运行中 · ") + LocalizationText(status.localization, status.quality);
    ImGui::TextColored(StatusColor(status.localization), "%s", lineOne.c_str());

    std::string lineTwo = std::string(GameStateText(status.gameState)) + "：";
    if (status.gameState == "bigMap") {
        lineTwo += std::to_string(status.mapMarkers) + " 个标记";
    }
    else if (status.gameState == "gameplay") {
        lineTwo += std::to_string(status.minimapMarkers) + " 个标记";
        if (status.localization == "stale") {
            lineTwo += " · 上次定位 " + std::to_string(status.lastGoodAgeMilliseconds / 100) + "." +
                std::to_string((status.lastGoodAgeMilliseconds / 10) % 10) + " 秒";
        }
		if (status.minimapMarkers == 0 && !status.message.empty()) {
			lineTwo += " · " + status.message;
		}
    }
    else {
        lineTwo += status.message;
    }
    if (status.frameMilliseconds > 0) lineTwo += " · " + std::to_string(status.frameMilliseconds) + " ms";
    ImGui::TextUnformatted(lineTwo.c_str());
    ImGui::End();
}
