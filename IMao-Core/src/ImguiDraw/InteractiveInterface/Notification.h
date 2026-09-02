#pragma once
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <utility>

enum class NotificationSeverity {
	Info,
	Error
};

class NotificationDatas 
{
public:
	std::string content;
	int timeDuration = 3;
	NotificationSeverity severity = NotificationSeverity::Info;

	NotificationDatas(std::string content, int timeDuration,
		NotificationSeverity severity = NotificationSeverity::Info)
		: content(std::move(content)), timeDuration(timeDuration), severity(severity) {}
};

class Notification
{
public:
	static void Start() {
		timerStopFlag = false;
		timerThread = std::thread(&Notification::Timer);
	}

	static void Stop() {
		timerStopFlag = true;
		if (timerThread.joinable()) timerThread.join();
	}
    static void DrawInfo();
	static void AddInfo(NotificationDatas addNotificationDatas);
	static void AddError(NotificationDatas addNotificationDatas);

private:
	static void Timer();
	static std::vector<NotificationDatas> notifications;
	static std::thread timerThread;
	static std::atomic_bool timerStopFlag;
	static std::mutex notificationsMutex;
};

