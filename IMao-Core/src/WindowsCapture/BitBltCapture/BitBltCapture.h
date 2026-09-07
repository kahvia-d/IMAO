#pragma once
#include <Windows.h>
#include <opencv2/opencv.hpp>
#include <memory>
class BitBltCapture
{
	public:
		BitBltCapture(HWND hwnd);
		 bool GetSnapshot_PrintWindow(cv::Mat& snapshot);
	private:
		 struct State;
		 HWND hwnd;
		 std::shared_ptr<State> state;
};

