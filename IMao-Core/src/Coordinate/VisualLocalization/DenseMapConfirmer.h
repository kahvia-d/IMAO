#pragma once

#include <opencv2/core.hpp>

#include <filesystem>
#include <string>

// Confirms a position we already trust by aligning the minimap against the map's own
// pixels, instead of asking the feature index again.
//
// Why this exists: the visual index answers "where am I" from a few hundred keypoints,
// and on flat terrain a single frame can carry far too few of them (measured on one
// player report: 30 keypoints where ordinary frames have 64-151). Coarse retrieval then
// finds no candidate at all and the tool sits in "recovering" while standing in the open.
// Where a prior already exists - the last tracked position, a big-map hint, or the
// game's own coordinate text - the map pixels are enough to say yes or no in a few
// milliseconds, and they do not care how few corners the terrain has.
//
// It never searches. The window is +-64 map pixels (about 53 world units) around the
// prior, a missing reference makes it inert, and it only ever confirms a position that
// some other part of the runtime already proposed.
namespace DenseMapConfirmer {
struct Result {
    bool available = false;  // false when the reference imagery or the inputs are missing
    bool accepted = false;
    double score = 0.0;                          // correlation at the prior
    double peakScore = 0.0;                      // best correlation in the window
    double peakOffsetX = 0.0, peakOffsetY = 0.0; // that peak, in map pixels, from the prior
    double milliseconds = 0.0;
    std::string detail;
};

// `normalizedMinimap` is the grey minimap the localizer already normalises (its own
// resolution decides the template size); `prior` is a map-space coordinate, the same
// space as VisualLocalizationCandidate::mapCenter; `terrainScale` is the scene's
// minimap scale.
Result Confirm(const cv::Mat& normalizedMinimap, int sceneId, const cv::Point2d& prior,
    double terrainScale);

// Same, against an explicit reference directory. The runtime uses the overload above;
// tests point this at a fixture so nothing has to be staged next to the executable.
Result Confirm(const cv::Mat& normalizedMinimap, int sceneId, const cv::Point2d& prior,
    double terrainScale, const std::filesystem::path& referenceRoot);

// The directory the reference tiles are read from, or an empty path when this
// installation ships no map imagery (which makes every call inert). Two places are
// tried: next to the executable, then the resource baseline root.
std::filesystem::path ReferenceRoot();

// Correlation at the prior that counts as a confirmation. Set from the offline replay of
// the reported frame: the correct prior scored 0.783 there, a prior 41 units off scored
// 0.56-0.64 and one 100 units off scored 0.011.
inline constexpr double MinimumScore = 0.68;
// How far the best peak may sit from the prior before the confirmation is refused: the
// terrain has to agree *at the prior*, not merely somewhere nearby.
inline constexpr double MaximumPeakOffsetPixels = 50.0;
}
