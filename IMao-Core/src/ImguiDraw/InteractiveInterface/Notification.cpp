#include "Notification.h"
#include "../ImGuiOverWindows.h"
#include "../../Diagnostics/Diagnostics.h"

#include <algorithm>

std::vector<NotificationDatas> Notification::notifications;
std::thread Notification::timerThread;
std::atomic_bool Notification::timerStopFlag = false;
std::mutex Notification::notificationsMutex;

namespace {
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration |
    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoMove;
}

void Notification::DrawInfo() {
    // Informational messages are logged, never drawn over the minimap.
    std::vector<NotificationDatas> errors;
    {
        std::scoped_lock lock(notificationsMutex);
        for (const auto& notification : notifications) {
            if (notification.severity == NotificationSeverity::Error && notification.timeDuration > 0) {
                errors.push_back(notification);
            }
        }
    }
    if (errors.empty()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float kPadding = 16.0f;
    const ImVec2 position(viewport->WorkPos.x + viewport->WorkSize.x - kPadding,
        viewport->WorkPos.y + viewport->WorkSize.y - kPadding);
    ImGui::SetNextWindowPos(position, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    bool open = true;
    if (ImGui::Begin("IMao Errors", &open, kWindowFlags)) {
        for (const auto& error : errors) {
            ImGui::TextUnformatted(error.content.c_str());
            ImGui::SameLine();
            ImGui::Text("(%ds)", error.timeDuration);
            ImGui::Separator();
        }
    }
    ImGui::End();
}

void Notification::AddInfo(NotificationDatas notification) {
    notification.severity = NotificationSeverity::Info;
    Diagnostics::Record("notification-info", notification.content);
}

void Notification::AddError(NotificationDatas notification) {
    notification.severity = NotificationSeverity::Error;
    Diagnostics::Record("notification-error", notification.content);
    std::scoped_lock lock(notificationsMutex);
    for (auto& existing : notifications) {
        if (existing.content == notification.content) {
            existing.timeDuration = notification.timeDuration;
            existing.severity = NotificationSeverity::Error;
            return;
        }
    }
    notifications.push_back(std::move(notification));
}

void Notification::Timer() {
    while (!timerStopFlag.load()) {
        try {
            {
                std::scoped_lock lock(notificationsMutex);
                for (auto& notification : notifications) --notification.timeDuration;
                std::erase_if(notifications, [](const NotificationDatas& notification) {
                    return notification.timeDuration <= 0;
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        catch (const std::exception& exception) {
            Diagnostics::Record("notification-timer-error", exception.what());
        }
    }
}
