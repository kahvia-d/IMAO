#include "../../App/App.h"
#include "LoadEditRouteData.h"
#include "DrawRouteOnMap.h"
#include "../Items/DrawItemOnGameMap.h"
#include <filesystem>
#include <shared_mutex>
#include <fstream>
#include "../../Runtime/AtomicFile.h"
#include "../../Runtime/UserFileName.h"
#include "../../Runtime/StructuredLogger.h"
#include "../InteractiveInterface/Notification.h"
using namespace std;
using namespace cv;

std::vector<RouteDatas> LoadEditRouteData::routesDatas;
App* LoadEditRouteData::app;
std::thread LoadEditRouteData::Thread;
std::atomic_bool LoadEditRouteData::threadStopFlag;
std::mutex LoadEditRouteData::dataMutex;
string LoadEditRouteData::routeJsonName = "Routes";
namespace {
const fs::path& RouteFolderPath() {
    static const fs::path path = [] {
        const auto destination = StructuredLogger::ApplicationDataDirectory() / "SavedRoutes";
        fs::create_directories(destination);
        const auto legacy = fs::path(GetCurrentPath()) / "SavedRoutes";
        if (fs::exists(legacy)) for (const auto& entry : fs::directory_iterator(legacy)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                fs::copy_file(entry.path(), destination / entry.path().filename(), fs::copy_options::skip_existing);
        }
        return destination;
    }();
    return path;
}
std::mutex routeFileMutex;
fs::path RoutePath(const std::string& name) {
    ValidateUserFileName(name);
    return RouteFolderPath() / fs::u8path(name + ".json");
}
}

void LoadEditRouteData::PrepareStorage() { (void)RouteFolderPath(); }

std::vector<RouteDatas> LoadEditRouteData::GetRoutesSnapshot() {
    std::scoped_lock lock(dataMutex);
    return routesDatas;
}


void LoadEditRouteData::Initi(App* app) {
	if (app != nullptr) {
		StopThread();
		LoadEditRouteData::app = app;
		try {
			fs::path folderPath(RouteFolderPath());

			if (!fs::exists(folderPath)) {
				fs::create_directories(folderPath);
			}
		}
		catch (const exception& e) {
			cerr << " LoadEditRouteData::Initi:" << e.what() << endl;
		}

		StartThread();
	}
}

void LoadEditRouteData::AddRouteDatas(string name ,int senceId, const Coordinate& ROC_a, const Coordinate& ROC_b) {
	vector<Coordinate> routePointsROC = GenerateEquidistantPoints(ROC_a, ROC_b, 5);
	{
		std::scoped_lock lock(dataMutex);
		LoadEditRouteData::routesDatas.push_back(RouteDatas(name, senceId, std::move(routePointsROC)));
	}
	DrawRouteOnMap::ClearRountsData();
}

void LoadEditRouteData::SetRouteJsonName(const string& setName) {
	ValidateUserFileName(setName);
	std::scoped_lock lock(dataMutex);
	routeJsonName = setName;
}

void LoadEditRouteData::Thread_KeyMonitoring_AddRouteDatas_ByMousePos() {
	const int monitoredKey = 0x51; //Q
	bool keyWasPressed = false;

	int state = 0;//0 准备 1 完成 
	Coordinate a_ROC; Coordinate b_ROC;
	int firstScene = 0;
	while (!threadStopFlag) {
		if (app == nullptr) return;

        Coordinate ROC;
        int senceId = 0;
        if (!app->TryGetRoutePoint(ROC, senceId)) {
            state = 0;
            keyWasPressed = false;
            Sleep(60);
            continue;
        }

		bool keyIsPressed = isKeyPressed(monitoredKey);
		if (keyIsPressed && !keyWasPressed) {
			keyWasPressed = true;

			if (state == 0 || firstScene != senceId) {
				firstScene = senceId;
				a_ROC = ROC;
				state = 1;
			}
			else {
				b_ROC = ROC;
				AddRouteDatas("name", senceId, a_ROC, b_ROC);
				std::string name;
				{ std::scoped_lock lock(dataMutex); name = routeJsonName; }
				WriteRoutesDatas(name, Scene::SceneIdToName(senceId), a_ROC, b_ROC);
				state = 0;
			}
		}
		else if (!keyIsPressed && keyWasPressed) {
			keyWasPressed = false;
		}
		Sleep(60);
	}
}

vector<json> LoadEditRouteData::ReadRoutesJson() {
    vector<json> result;
    std::scoped_lock lock(routeFileMutex);
    if (!fs::exists(RouteFolderPath())) return result;
    for (const auto& file : fs::directory_iterator(RouteFolderPath())) {
        if (!file.is_regular_file() || file.path().extension() != ".json") continue;
        // Fail the batch before replacing the visible routes if any file is corrupt.
        ifstream input(file.path());
        result.push_back(json::parse(input));
    }
    return result;
}

vector<json> LoadEditRouteData::ReadRoutesJson(string routeName) {
    std::scoped_lock lock(routeFileMutex);
    ifstream input(RoutePath(routeName));
    if (!input) throw std::runtime_error("无法读取指定路线文件");
    return { json::parse(input) };
}

void LoadEditRouteData::LoadRoutesDatasFromLocal(bool isLoadAll, string routeName) {
    const auto documents = isLoadAll ? ReadRoutesJson() : ReadRoutesJson(routeName);
    vector<RouteDatas> next;
    for (const auto& document : documents) {
        if (!document.is_object()) throw std::runtime_error("路线必须是场景对象");
        for (const auto& [scene, segments] : document.items()) {
            const int sceneId = Scene::SceneNameToId(scene.c_str());
            if (sceneId <= 0 || !segments.is_array()) throw std::runtime_error("路线场景或线段格式无效");
            for (const auto& segment : segments) {
                if (!segment.is_array() || segment.size() != 2) throw std::runtime_error("路线线段必须包含两个端点");
                Coordinate points[2];
                for (size_t i = 0; i < 2; ++i) {
                    if (!segment[i].is_array() || segment[i].size() != 2 ||
                        !segment[i][0].is_number() || !segment[i][1].is_number())
                        throw std::runtime_error("路线端点必须包含两个数值坐标");
                    points[i] = {segment[i][0].get<double>(), segment[i][1].get<double>()};
                    if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) throw std::runtime_error("路线坐标必须为有限数值");
                }
                if (CalculatePointDistance(points[0], points[1]) > 100000) throw std::runtime_error("路线线段过长");
                // The origin (0,0) is a valid endpoint; do not use it as a missing-value sentinel.
                next.emplace_back(scene, sceneId, GenerateEquidistantPoints(points[0], points[1], 5));
            }
        }
    }
    {
        std::scoped_lock lock(dataMutex);
        if (isLoadAll) routesDatas = std::move(next);
        else routesDatas.insert(routesDatas.end(), next.begin(), next.end());
    }
    DrawRouteOnMap::ClearRountsData();
}

void LoadEditRouteData::WriteRoutesDatas(const string& routeFileName,const string& senceName, const Coordinate& ROC_a, const Coordinate& ROC_b) {
	std::scoped_lock lock(routeFileMutex);
	const fs::path jsonRoutePath = RoutePath(routeFileName);

	try {
		json j;
		{
			ifstream routeJson(jsonRoutePath);
			if (routeJson.is_open()) routeJson >> j;
		}

		j[senceName].push_back({ {ROC_a.x,ROC_a.y},{ROC_b.x,ROC_b.y} });

		WriteTextAtomically(jsonRoutePath, j.dump(4));
	}
	catch (const exception& ex) {
		cerr << "WriteRount:" << ex.what() << endl;
		Notification::AddError(NotificationDatas(" LoadEditRouteData::WriteRoutesDatas: " + string(ex.what()), 5));
	}
}
