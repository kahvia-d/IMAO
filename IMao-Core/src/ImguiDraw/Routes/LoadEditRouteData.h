#pragma once
#include "vector"
#include "../../Coordinate/CoordinateStruct.h"
#include "../../util.h"
#include "../../Domain/MapData.h"
#include <atomic>
#include <mutex>
class App;
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class LoadEditRouteData {
public:
	static std::vector<RouteDatas> GetRoutesSnapshot();
	static void PrepareStorage();
	static void Initi(App* app);
	static void AddRouteDatas(std::string name, int senceId, const Coordinate& ROC_a, const Coordinate& ROC_b);
	static void SetRouteJsonName(const std::string& setName);
	static void Thread_KeyMonitoring_AddRouteDatas_ByMousePos();
	static std::vector<json> ReadRoutesJson();
	static std::vector<json> ReadRoutesJson(std::string routeName);
	static void LoadRoutesDatasFromLocal(bool isLoadAll, std::string routeName);
	static void WriteRoutesDatas(const std::string& routeFileName,const std::string& senceName, const Coordinate& ROC_a, const Coordinate& ROC_b);

	static void StopThread() {
		threadStopFlag = true;
		if (Thread.joinable()) Thread.join();
		app = nullptr;
	}

	static void ClearRoutesDatas() {
		std::scoped_lock lock(dataMutex);
		routesDatas.clear();
	}
private:
	static std::vector<RouteDatas> routesDatas;
	static std::mutex dataMutex;
	static void StartThread() {
		threadStopFlag = false;
		Thread = std::thread(&LoadEditRouteData::Thread_KeyMonitoring_AddRouteDatas_ByMousePos);
	}

	static App* app;
	static std::thread Thread;
	static std::atomic_bool threadStopFlag;
	static std::string routeJsonName;
};
