#include "CoordinateRowAlignment.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

namespace CoordinateYolo {
namespace {
std::vector<cv::Mat> ReadBank(const nlohmann::json& bank) {
    std::vector<cv::Mat> result;
    for (const char* key : {"0", "1", "2", "8"}) {
        const auto data = bank.at(key).get<std::vector<float>>();
        if (data.size() != 32*48 || std::any_of(data.begin(), data.end(), [](float v){ return !std::isfinite(v); }))
            throw std::runtime_error("invalid coordinate glyph template");
        cv::Mat glyph(48, 32, CV_32F);
        std::copy(data.begin(), data.end(), glyph.ptr<float>());
        result.push_back(std::move(glyph));
    }
    return result;
}

RowFit FitBank(const cv::Mat& crop, const std::vector<cv::Mat>& templates, int preferredHeight = 0, const RowFit* hint = nullptr) {
    cv::Mat gray;
    if(crop.channels()==1)gray=crop;
    else cv::cvtColor(crop,gray,crop.channels()==4?cv::COLOR_BGRA2GRAY:cv::COLOR_BGR2GRAY);
    // Bound the template search cost on very high resolution captures.
    const double scale=std::min(1.,48./gray.rows);
    cv::Mat sampled;
    if(scale<1)cv::resize(gray,sampled,{},scale,scale,cv::INTER_AREA);
    else sampled=gray;
    cv::Mat detail,background,padded;
    sampled.convertTo(detail,CV_32F);
    cv::GaussianBlur(detail,background,{},std::max(1.,sampled.rows/16.));
    detail-=background;
    const int margin=std::max(2,sampled.rows/4);
    cv::copyMakeBorder(detail,padded,margin,margin,0,0,cv::BORDER_REPLICATE);
    RowFit best;
    int first = preferredHeight ? preferredHeight : std::max(8, cvRound(sampled.rows * .54));
    int last = preferredHeight ? preferredHeight : cvRound(sampled.rows * .91);
    if (hint) {
        const int center = cvRound(hint->height * scale);
        first = std::max(first, center - 2);
        last = std::min(last, center + 2);
    }
    if (last < first) return best;
    std::vector<RowFit> heights(last - first + 1);
    const auto evaluate = [&](const cv::Range& range) {
        for (int height = range.start; height < range.end; ++height) {
            const cv::Size size(cvRound(height * 26. / 36.), height);
            if (size.width > sampled.cols || size.height > padded.rows) continue;
            const int center = hint ? cvRound(hint->top * scale) + margin : 0;
            const int y0 = hint ? std::clamp(center - 3, 0, padded.rows - height) : 0;
            const int y1 = hint ? std::clamp(center + 3, y0, padded.rows - height) : padded.rows - height;
            const auto search = padded.rowRange(y0, y1 + height);
            cv::Mat sum, minimum;
            for (const auto& glyph : templates) {
                cv::Mat stamp, correlation, peak;
                cv::resize(glyph, stamp, size, 0, 0, cv::INTER_AREA);
                cv::matchTemplate(search, stamp, correlation, cv::TM_CCOEFF_NORMED);
                cv::reduce(correlation, peak, 1, cv::REDUCE_MAX);
                if (sum.empty()) {
                    sum = peak.clone();
                    minimum = peak.clone();
                } else {
                    sum += peak;
                    cv::min(minimum, peak, minimum);
                }
            }
            // The best three glyphs must support the same row and scale.
            const cv::Mat confidence = (sum - minimum) / 3;
            double score;
            cv::Point peak;
            cv::minMaxLoc(confidence, nullptr, &score, nullptr, &peak);
            heights[height - first] = {(peak.y + y0 - margin) / scale, height / scale, score};
        }
    };
    const cv::Range range(first, last + 1);
    if (first == last) evaluate(range);
    else cv::parallel_for_(range, evaluate, 4);
    // Reduce in ascending height order to keep tie-breaking deterministic.
    for (const auto& fit : heights) {
        if (fit.score > best.score) best = fit;
    }
    return best;
}

}

RowAlignment::RowAlignment(const std::filesystem::path& directory) {
    const auto path = directory / L"model.json";
    if (!std::filesystem::exists(path)) return;
    std::ifstream file(path);
    nlohmann::json metadata;
    file >> metadata;
    if (!metadata.contains("rowAlignment")) return;
    const auto& recipe = metadata.at("rowAlignment");
    if (recipe.at("version") != 1) throw std::runtime_error("unsupported coordinate row alignment version");
    templates_ = ReadBank(recipe.at("templates"));
    brightTemplates_ = ReadBank(recipe.at("brightTemplates"));
}

RowFit RowAlignment::Fit(const cv::Mat& crop, bool hasDetections, const RowFit* hint) const {
    // On bright scenery the shadow around a digit is often stronger than its fill.
    // Try the expected size first; a weak match still searches the full range.
    const double scale = std::min(1., 48./crop.rows);
    const int expected = hint ? cvRound(hint->height * scale) : cvRound(std::min(crop.rows,48)*.75);
    auto fit = FitBank(crop, templates_, expected, hint);
    if (fit.score >= .74) return fit;
    const auto bright = FitBank(crop, brightTemplates_, expected, hint);
    if (bright.score >= .74) return bright;
    if (hint && fit.top == bright.top && std::abs(hint->top - fit.top) * scale <= 1 &&
        std::max(fit.score, bright.score) >= .60) return fit.score >= bright.score ? fit : bright;
    // Allow an empty detection to recover when either bank sees a plausible row.
    if (!hasDetections && std::max(fit.score, bright.score) < .50) return {};
    if (hint) {
        fit = FitBank(crop, templates_, 0, hint);
        if (fit.score >= .65) return fit;
        const auto alternate = FitBank(crop, brightTemplates_, 0, hint);
        if (alternate.score > fit.score) fit = alternate;
        if (fit.score >= .35) return fit;
    }
    fit = FitBank(crop, templates_);
    if (fit.score < .65) {
        const auto alternate = FitBank(crop, brightTemplates_);
        if (alternate.score > fit.score) fit = alternate;
    }
    return fit;
}

cv::Mat RowAlignment::Resample(const cv::Mat& crop,const RowFit& fit,cv::Point2d phase) {
    const double gain=36./fit.height;
    const cv::Matx23f transform(static_cast<float>(gain),0,static_cast<float>(phase.x),
        0,static_cast<float>(gain),static_cast<float>(3+phase.y-fit.top*gain));
    cv::Mat aligned;
    cv::warpAffine(crop,aligned,transform,{884,48},cv::INTER_CUBIC,cv::BORDER_REPLICATE);
    return aligned;
}

}
