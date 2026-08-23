#include "IdentifyWorldCoordinates.h"
#include "..\..\util.h"
#include "..\locationCalculator\ScreenCoordinate.h"
#include "../../Diagnostics/Diagnostics.h"

#include <regex>
#include <sstream>
using namespace std;
std::unique_ptr<PPOCR> IdentifyWorldCoordinates::p_ppocr;
bool IdentifyWorldCoordinates::isLoaded = false;

void IdentifyWorldCoordinates::Init(string det_model_dir, string rec_model_dir,string rec_char_dict_path,string cls_model_dir) {
	FLAGS_det_model_dir = det_model_dir;
	FLAGS_rec_model_dir = rec_model_dir;
	if (!rec_char_dict_path.empty()) {
		FLAGS_rec_char_dict_path = rec_char_dict_path;
	}
	FLAGS_cls_model_dir = cls_model_dir;
	p_ppocr = make_unique<PPOCR>();
	isLoaded = true;
}

bool IsValidSingleOCRForCoords(const vector<OCRPredictResult>& ocr_result, string& outWorldCoordinateText) {

	// PaddleOCR sometimes returns the correct coordinate together with an
	// empty, zero-confidence detection (or a low-confidence stray glyph).
	// Count only meaningful high-confidence candidates; accepting raw vector
	// size here made valid coordinates fail intermittently on the current UI.
	const OCRPredictResult* coordinateCandidate = nullptr;
	for (const auto& result : ocr_result) {
		const bool hasText = result.text.find_first_not_of(" \t\r\n") != string::npos;
		if (result.score > 0.85 && hasText) {
			if (coordinateCandidate != nullptr) {
				return false;
			}
			coordinateCandidate = &result;
		}
	}

	if (coordinateCandidate == nullptr) {
		return false;
	}

	string ocr_textResult = UTF8ToGBK(coordinateCandidate->text);

	int i = 0;
	for (char& c : ocr_textResult) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			return false;
		}

		if (c == '.' || c == ',' || c == ':') {
			c = '|';
			i++;
		}
	}

	if (i != 2) {
		// The current game font can make the first comma look like '-' to the
		// bundled OCR model (for example, "-409-561.49"), or occasionally drop
		// the second separator ("-409-56149").  These two forms are
		// unambiguous because the first value has its own leading sign and the
		// altitude is always the final two digits.  Normalize them back into the
		// legacy three-field representation before rejecting the capture.
		std::smatch match;
		static const std::regex commaReadAsDash(R"(^(-[0-9]+)-([0-9]+)[\.,:]([0-9]+)$)");
		static const std::regex missingFinalSeparator(R"(^(-[0-9]+)-([0-9]+)([0-9]{2})$)");
		if (std::regex_match(ocr_textResult, match, commaReadAsDash) ||
			std::regex_match(ocr_textResult, match, missingFinalSeparator)) {
			outWorldCoordinateText = match[1].str() + "|" + match[2].str() + "|" + match[3].str();
			return true;
		}
		return false;
	}

	outWorldCoordinateText = ocr_textResult;
	return true;
}


vector<int> SplitStringToIntArray(const string& input) {
	std::vector<int> result;
	std::stringstream ss(input);
	std::string token;

	while (std::getline(ss, token, '|')) {
		try {
			result.push_back(std::stoi(token));
		}
		catch (const std::invalid_argument& e) {
			std::cerr << "输入的数字格式无效: " << token << std::endl;
		}
		catch (const std::out_of_range& e) {
			std::cerr << "数字超出范围: " << token << std::endl;
		}
	}
	return result;
}


bool IdentifyWorldCoordinates::IdentifyCoordinate(const Mat& imgCoordinates,Coordinate& outPlayerWorldCoordinate) {;
	cout << "!!ocr识别!!" ;
	vector<OCRPredictResult> ocr_result = p_ppocr->ocr(imgCoordinates, true, true, true);
	string playerWorldCoordinate;
	std::ostringstream details;
	details << "count=" << ocr_result.size();
	for (size_t index = 0; index < ocr_result.size(); ++index) {
		details << " result" << index << "='" << ocr_result[index].text << "' score=" << ocr_result[index].score;
	}
	if (IsValidSingleOCRForCoords(ocr_result, playerWorldCoordinate)) {
		vector<int> pWorldCoordinateArr= SplitStringToIntArray(playerWorldCoordinate);
		if (pWorldCoordinateArr.size() == 3) {
			Coordinate playerWorldCoordinate(pWorldCoordinateArr[0], pWorldCoordinateArr[1]);
			outPlayerWorldCoordinate = playerWorldCoordinate;
			Diagnostics::Record("ocr-result", details.str() + " accepted=true coordinate=" + std::to_string(outPlayerWorldCoordinate.x) + "," + std::to_string(outPlayerWorldCoordinate.y));
			return true;
		}
	}
	Diagnostics::Record("ocr-result", details.str() + " accepted=false");
	return false;
}

bool IdentifyWorldCoordinates::IdentifyCoordinateFromSnapshot(const Mat& snapshot, Coordinate& outPlayerWorldCoordinate, RECT rect) {
	Mat imgCoordinates = ImageProcessing::CropToShowWorldCoordinateAreaImg(snapshot, rect);
	if (imgCoordinates.empty()) {
		Diagnostics::Record("ocr-crop", "coordinate crop is empty");
		return false;
	}
	Diagnostics::SaveImage("ocr-coordinate-crop", imgCoordinates);

	imgCoordinates = ImageProcessing::centerAndScaleImage(imgCoordinates, 2);
	Diagnostics::SaveImage("ocr-coordinate-scaled", imgCoordinates);
	return IdentifyCoordinate(imgCoordinates, outPlayerWorldCoordinate);
}

bool IdentifyWorldCoordinates::IdentifyCoordinateFromSnapshot(const Mat& snapshot, Coordinate& outPlayerWorldCoordinate, HWND w_hwnd) {
	Mat imgCoordinates = ImageProcessing::CropToShowWorldCoordinateAreaImg(snapshot, w_hwnd);
	if (imgCoordinates.empty())
		return false;

	imgCoordinates = ImageProcessing::centerAndScaleImage(imgCoordinates, 2);

	return IdentifyCoordinate(imgCoordinates, outPlayerWorldCoordinate);
}
