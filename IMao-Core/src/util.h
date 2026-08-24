#pragma once
#include <opencv2/opencv.hpp>
#include "pch.h"
#include <vector>
#include <thread>
#include <include/paddleocr.h>
#include <include/paddlestructure.h>
#include <opencv2/features2d.hpp> 
#include <Windows.h>
#include "Coordinate\CoordinateStruct.h"
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

struct NonClientRegion {
	int non_client_height_total;
	int non_client_width_total;
};

struct ScreenPoint {
    int x;
    int y;
};


struct WindowCorners {
    ScreenPoint topLeft;
    ScreenPoint topRight;
    ScreenPoint bottomLeft;
    ScreenPoint bottomRight;
};


struct WindowSize {
    int height;
    int width;
};

std::string UTF8ToGBK(const std::string& utf8);

HWND GetWindowHandleByTitle(const wchar_t* lpClassName, const wchar_t* windowTitle);

bool CalculateNonClientAreaSize(const HWND& hwnd, NonClientRegion& nonClientRegion);

bool CalculateWindowScalingFactors(const HWND& hwnd,double& HorizontalFactor,double& VerticaFactor);
bool CalculateWindowScalingFactors(const RECT& w_Rect, double& HorizontalFactor, double& VerticaFactor);
bool IsWindowFocused(HWND windowHwnd);

WindowCorners GetWindowClientCorners(HWND hwnd);

float CalculatePointDistance(Coordinate a, Coordinate b);

std::string GetCurrentPath();

std::string ReplaceSlashes(const std::string& input);

HWND GetWindowHandleByProcessName(const wchar_t* processName);

// Returns true only for a visible, non-minimized game client window with a
// usable capture surface.  The game process can temporarily expose helper
// windows (including 0x0 windows) while it is starting or closing; those must
// never be used as the capture/overlay target.
bool GetUsableClientRect(HWND hwnd, RECT& clientRect);

bool isKeyPressed(int keyCode);

double DistanceBetweenPoints(const Coordinate& p1, const Coordinate& p2);

std::vector<Coordinate> GenerateEquidistantPoints(const Coordinate& start, const Coordinate& end, double step);

std::vector<fs::path> findFilesByExtensions(const std::string& folderPath, const std::unordered_set<std::string>& targetExtensions);
