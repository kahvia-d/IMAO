#include "BitBltCapture.h"
#include <iostream>
#include <mutex>
#include <wil/resource.h>
using namespace std;
using namespace cv;

struct BitBltCapture::State {
	std::mutex mutex;
	HDC memoryDc = nullptr;
	HBITMAP bitmap = nullptr;
	HGDIOBJ previousBitmap = nullptr;
	void* pixels = nullptr;
	int width = 0;
	int height = 0;
	DWORD ownerThread = 0;

	~State() { Reset(); }

	void Reset() {
		if (memoryDc && previousBitmap) SelectObject(memoryDc, previousBitmap);
		if (bitmap) DeleteObject(bitmap);
		if (memoryDc) DeleteDC(memoryDc);
		memoryDc = nullptr;
		bitmap = nullptr;
		previousBitmap = nullptr;
		pixels = nullptr;
		width = height = 0;
		ownerThread = 0;
	}

	bool EnsureBuffer(HDC sourceDc, int requestedWidth, int requestedHeight) {
		const auto threadId = GetCurrentThreadId();
		if (memoryDc && width == requestedWidth && height == requestedHeight &&
			ownerThread == threadId) return true;

		// Startup probes run on the runtime thread; continuous capture runs on
		// its own worker. Recreate the GDI resources when ownership changes.
		Reset();
		memoryDc = CreateCompatibleDC(sourceDc);
		if (!memoryDc) return false;

		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = requestedWidth;
		info.bmiHeader.biHeight = -requestedHeight;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		bitmap = CreateDIBSection(sourceDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		if (!bitmap || !pixels) {
			Reset();
			return false;
		}
		previousBitmap = SelectObject(memoryDc, bitmap);
		if (!previousBitmap || previousBitmap == HGDI_ERROR) {
			previousBitmap = nullptr;
			Reset();
			return false;
		}
		width = requestedWidth;
		height = requestedHeight;
		ownerThread = threadId;
		return true;
	}
};

BitBltCapture::BitBltCapture(HWND hwnd) : hwnd(hwnd), state(std::make_shared<State>()) {}

bool BitBltCapture::GetSnapshot_PrintWindow(Mat& snapshot) {
	snapshot.release();
	if (!IsWindow(hwnd)) {
		return false;
	}

	RECT clientRect{};
	if (!GetClientRect(hwnd, &clientRect)) return false;
	int width = clientRect.right - clientRect.left;
	int height = clientRect.bottom - clientRect.top;
	if (width <= 0 || height <= 0) return false;
	// The capture object is copied into App through std::optional. Copies
	// share one buffer and must never draw into it concurrently.
	std::lock_guard lock(state->mutex);

	HDC hdcScreen = GetDC(hwnd);
	if (!hdcScreen) {
		return false;
	}
	auto releaseScreen = wil::scope_exit([&] { ReleaseDC(hwnd, hdcScreen); });

	if (!state->EnsureBuffer(hdcScreen, width, height)) return false;
	BOOL result = PrintWindow(hwnd, state->memoryDc, 3);
	if (result == 0) {
		result = BitBlt(state->memoryDc, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY | CAPTUREBLT);
	}
	if (result != 0) {
		// Flush the GDI writes before reading the DIB memory. cvtColor creates
		// an owned BGR image, so published snapshots cannot alias the next draw.
		if (!GdiFlush()) return false;
		const cv::Mat image(height, width, CV_8UC4, state->pixels,
			static_cast<size_t>(width) * 4);
		cvtColor(image, snapshot, cv::COLOR_BGRA2BGR);
	}
	return result != 0;
}
