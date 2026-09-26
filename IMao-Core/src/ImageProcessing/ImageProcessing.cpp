#include "ImageProcessing.h"
#include "../Coordinate/locationCalculator/ScreenCoordinate.h"
#include "../util.h"
using namespace cv;

Mat ImageProcessing::extractCircularRegionFromImage(Mat img,Point center,int radius) {
	Mat mask = Mat::zeros(img.size(), CV_8UC1);
	circle(mask, center, radius, Scalar(255), FILLED);
	
	Mat circularRegionImg;
	img.copyTo(circularRegionImg, mask);

	return circularRegionImg;
}

Mat ImageProcessing::imgToGray(Mat img) {
	Mat ImgGray;
	cvtColor(img, ImgGray, COLOR_BGR2GRAY);

	return ImgGray;
}

Mat ImageProcessing::cropImageWithRect(Mat img, Rect roi) {
	// A capture can briefly lag the client rect while the game window resizes, and the callers treat
	// an empty result as "no pixels this frame". Throwing out of the unchecked ROI constructor used to
	// take the whole worker thread with it.
	const Rect bounds(0, 0, img.cols, img.rows);
	if (roi.width <= 0 || roi.height <= 0 || (roi & bounds) != roi) return {};
	Mat croppedImage = img(roi);
	return croppedImage;
}


Mat ImageProcessing::increaseImageResolution(Mat image, float scaleFactor) {
	int newWidth = image.cols * scaleFactor;
	int newHeight = image.rows * scaleFactor;

	cv::Mat resizedImage;

	cv::resize(image, resizedImage, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_CUBIC);

	return resizedImage;
}


Mat ImageProcessing::centerAndScaleImage(Mat img, double scaleFactor) {

	// 设定目标图像尺寸（根据需求调整）
	int targetWidth = 1000;  // 目标宽度
	int targetHeight = 500; // 目标高度
	cv::Mat blackBG(targetHeight, targetWidth, CV_8UC3, cv::Scalar(0, 0, 0)); // 黑色背景

	// 计算粘贴位置（示例为居中，可自定义）
	int x = (targetWidth - img.cols) / 2;
	int y = (targetHeight - img.rows) / 2;

	// 确保裁剪区域尺寸不超过目标尺寸
	if (x >= 0 && y >= 0 && x + img.cols <= targetWidth && y + img.rows <= targetHeight) {
		img.copyTo(blackBG(cv::Rect(x, y, img.cols, img.rows)));
	}

	// 计算缩放中心
	cv::Point2f center(blackBG.cols / 2.0, blackBG.rows / 2.0);

	// 获取仿射变换矩阵，围绕中心缩放
	cv::Mat rotationMatrix = cv::getRotationMatrix2D(center, 0, scaleFactor);

	// 创建一个用于存储缩放后图片的矩阵
	cv::Mat scaledImage;

	// 应用仿射变换
	cv::warpAffine(blackBG, scaledImage, rotationMatrix, blackBG.size(), cv::INTER_NEAREST);

	return scaledImage;
}


Mat ImageProcessing::CropToShowWorldCoordinateAreaImg(const Mat& snapshot, const RECT& w_Rect) {
    Mat showWorldCoordinateAreaImg;

	const Rect roi = ScreenCoordinate::ScreenRect(w_Rect, hud::kCoordinateReadout);

	showWorldCoordinateAreaImg = ImageProcessing::cropImageWithRect(snapshot, roi);

	return showWorldCoordinateAreaImg;
}


Mat ImageProcessing::CropToShowWorldCoordinateAreaImg(const Mat& snapshot, const HWND hwnd) {
	Mat showWorldCoordinateAreaImg;
	RECT w_Rect;
	GetClientRect(hwnd, &w_Rect);

	showWorldCoordinateAreaImg = CropToShowWorldCoordinateAreaImg(snapshot, w_Rect);

	return showWorldCoordinateAreaImg;
}


Mat ImageProcessing::CropToMinMapAreaImg(const Mat& snapshot, const RECT& w_Rect, Coordinate& minMapBottomPoint) {
	Mat circularRegionImg;

	const Rect roi = ScreenCoordinate::ScreenRect(w_Rect, hud::kMinimap);
	Mat croppedImage = ImageProcessing::cropImageWithRect(snapshot, roi);
	if (croppedImage.empty()) return circularRegionImg;
	// The circle has to be described in the crop's own terms: centre = (columns/2, rows/2) and a
	// radius the shorter side can hold. Swapping the two and taking the radius from the columns alone
	// is invisible on a square crop and erases the bottom of the minimap on any other one (a
	// 2560x1600 client crops 246x246 now, but a stale client rect or a future layout is not square).
	Point center(croppedImage.cols / 2, croppedImage.rows / 2);
	int radius = (std::min)(croppedImage.cols, croppedImage.rows) / 2 - 3;
	circularRegionImg = ImageProcessing::extractCircularRegionFromImage(croppedImage, center, radius);

	minMapBottomPoint = Coordinate(roi.x + roi.width / 2, roi.y + roi.height);
	return circularRegionImg;
}


//Mat ImageProcessing::CropToMinMapAreaImg(const Mat& snapshot, HWND& hwnd) {
//	Mat circularRegionImg;
//	RECT w_Rect;
//	GetClientRect(hwnd, &w_Rect);
//
//	circularRegionImg = CropToMinMapAreaImg(snapshot, w_Rect);
//	return circularRegionImg;
//}


Mat ImageProcessing::CropToRegion_IconTask(const Mat& snapshot,const RECT& w_Rect) {

	const Rect roi = ScreenCoordinate::ScreenRect(w_Rect, hud::kTaskIcon);
	Mat croppedImage = ImageProcessing::cropImageWithRect(snapshot, roi);

	return croppedImage;
}

Mat ImageProcessing::CropToRegion_IconWavePlateCrystal(const Mat& snapshot, const RECT& w_Rect) {

	const Rect roi = ScreenCoordinate::ScreenRect(w_Rect, hud::kWavePlateCrystal);
	Mat croppedImage = ImageProcessing::cropImageWithRect(snapshot, roi);
	
	return croppedImage;
}

Mat ImageProcessing::CropToMapCenterArea(const Mat& snapshot, const RECT& w_Rect) {
	
	const Rect roi = ScreenCoordinate::ScreenRect(w_Rect, hud::kMapCenterArea);
	Mat croppedImage = ImageProcessing::cropImageWithRect(snapshot, roi);

	return croppedImage;
}

Mat ImageProcessing::CropToWindowsClientArea(const Mat& snapshot,const NonClientRegion& nonClientRegion,const RECT& rect) {
	Rect roi(nonClientRegion.non_client_width_total, nonClientRegion.non_client_height_total, rect.right, rect.bottom);
	Mat croppedImage = ImageProcessing::cropImageWithRect(snapshot,roi);
	return croppedImage;
}
