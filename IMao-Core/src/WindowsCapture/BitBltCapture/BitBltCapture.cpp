#include "BitBltCapture.h"
#include <iostream>
#include <wil/resource.h>
using namespace std;
using namespace cv;

bool BitBltCapture::GetSnapshot_PrintWindow(Mat& snapshot) {
	if (!IsWindow(hwnd)) {
		return false;
	}

	RECT clientRect;
	GetClientRect(hwnd, &clientRect);
	int width = clientRect.right - clientRect.left;
	int height = clientRect.bottom - clientRect.top;

	HDC hdcScreen = GetDC(hwnd);
	if (!hdcScreen) {
		return false;
	}
	auto releaseScreen = wil::scope_exit([&] { ReleaseDC(hwnd, hdcScreen); });

	// 创建兼容DC和位图
	HDC hdcMemory = CreateCompatibleDC(hdcScreen);
	HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);

	if (!hdcMemory || !hBitmap) {
		if (hBitmap) DeleteObject(hBitmap);
		if (hdcMemory) DeleteDC(hdcMemory);
		return false;
	}
	auto deleteMemory = wil::scope_exit([&] { DeleteDC(hdcMemory); });
	auto deleteBitmap = wil::scope_exit([&] { DeleteObject(hBitmap); });

	// 选择位图到内存DC
	HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMemory, hBitmap);
	if (!hOldBitmap || hOldBitmap == HGDI_ERROR) {
		return false;
	}
	auto restoreSelection = wil::scope_exit([&] { SelectObject(hdcMemory, hOldBitmap); });
	BOOL result = PrintWindow(hwnd, hdcMemory, 3);
	if (result == 0) {
		result = BitBlt(hdcMemory, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY | CAPTUREBLT);
	}
	if (result != 0) {
		BITMAPINFOHEADER bi;
		bi.biSize = sizeof(BITMAPINFOHEADER);
		bi.biWidth = width;
		bi.biHeight = -height;  // 负值表示自上而下的DIB
		bi.biPlanes = 1;
		bi.biBitCount = 32;
		bi.biCompression = BI_RGB;
		bi.biSizeImage = 0;
		bi.biXPelsPerMeter = 0;
		bi.biYPelsPerMeter = 0;
		bi.biClrUsed = 0;
		bi.biClrImportant = 0;

		cv::Mat image(height, width, CV_8UC4);

		if (GetDIBits(hdcMemory, hBitmap, 0, height, image.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS) == 0) {
			return false;
		}

		cvtColor(image, snapshot, cv::COLOR_BGRA2BGR);
	}
	return result != 0;
}
