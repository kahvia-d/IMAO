// Rewrites v1 feature binaries in place as IMAOFT01 v2.
//
// A pack's feature source (the XML the converter normally consumes) is not
// shipped with the packs, so the shipping format is reached by re-encoding the
// binaries themselves: read the v1 file, quantize and deflate it, verify that
// every keypoint survived and every descriptor stayed inside the declared
// quantization bound, and only then replace the file. The source hash recorded
// in the file carries over unchanged, because the source it identifies is the
// same one the pack was built from.
#include "Feature/Processing/FeatureBinaryCodec.h"

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

bool AtomicReplace(const fs::path& temporary, const fs::path& destination) {
    return MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool Acceptable(const ImageFeatureData& before, const ImageFeatureData& after,
    float tolerance, std::string& error) {
    if (before.imgKeypoints.size() != after.imgKeypoints.size() ||
        before.imgDescriptors.rows != after.imgDescriptors.rows) {
        error = "keypoint count changed during migration";
        return false;
    }
    for (std::size_t index = 0; index < before.imgKeypoints.size(); ++index) {
        const auto& left = before.imgKeypoints[index];
        const auto& right = after.imgKeypoints[index];
        if (left.pt.x != right.pt.x || left.pt.y != right.pt.y || left.size != right.size ||
            left.angle != right.angle || left.response != right.response ||
            left.octave != right.octave || left.class_id != right.class_id) {
            error = "keypoint " + std::to_string(index) + " changed during migration";
            return false;
        }
    }
    const auto* expected = before.imgDescriptors.ptr<float>();
    const auto* actual = after.imgDescriptors.ptr<float>();
    const auto valueCount = static_cast<std::size_t>(before.imgDescriptors.total());
    float worst = 0.0f;
    for (std::size_t index = 0; index < valueCount; ++index) {
        const auto difference = std::fabs(expected[index] - actual[index]);
        if (difference > worst) worst = difference;
    }
    if (worst > tolerance) {
        error = "descriptor deviation " + std::to_string(worst) +
            " exceeds the quantization bound " + std::to_string(tolerance);
        return false;
    }
    return true;
}

/// Points the pack's own manifest at the record the rewritten binary now carries,
/// so the version and payload hash a reader would trust stay truthful.
void UpdateSidecar(const fs::path& binary, const FeatureBinaryHeader& header, std::string& error) {
    const auto sidecar = fs::path(binary.wstring() + L".manifest.json");
    if (!fs::exists(sidecar)) return;
    std::ifstream input(sidecar);
    if (!input) {
        error = "sidecar cannot be opened: " + sidecar.string();
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    const auto replaceNumber = [&text](const std::string& key, const std::string& value) {
        const auto at = text.find("\"" + key + "\"");
        if (at == std::string::npos) return text;
        const auto colon = text.find(':', at);
        if (colon == std::string::npos) return text;
        auto end = text.find_first_of(",}\n", colon);
        if (end == std::string::npos) end = text.size();
        return text.substr(0, colon + 1) + " " + value + text.substr(end);
    };

    std::string updated = replaceNumber("version", std::to_string(header.version));
    updated = replaceNumber("payloadSha256", "\"" + FeatureBinaryCodec::Sha256Hex(header.payloadSha256) + "\"");

    std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "sidecar cannot be rewritten: " + sidecar.string();
        return;
    }
    output << updated;
    if (!output) error = "sidecar write failed: " + sidecar.string();
}

struct Totals {
    std::uint64_t files = 0;
    std::uint64_t skipped = 0;
    std::uint64_t failed = 0;
    std::uint64_t keypoints = 0;
    std::uint64_t bytesBefore = 0;
    std::uint64_t bytesAfter = 0;
};

bool MigrateFile(const fs::path& binary, Totals& totals, bool apply) {
    std::error_code sizeError;
    const auto bytesBefore = fs::file_size(binary, sizeError);
    if (sizeError) {
        std::cerr << "  cannot size " << binary.string() << ": " << sizeError.message() << '\n';
        ++totals.failed;
        return false;
    }

    ImageFeatureData features;
    FeatureBinaryHeader header;
    std::string error;
    if (!FeatureBinaryCodec::Load(binary, features, error, &header)) {
        std::cerr << "  cannot read " << binary.string() << ": " << error << '\n';
        ++totals.failed;
        return false;
    }
    if (header.version >= FeatureBinaryHeader::CurrentVersion) {
        ++totals.skipped;
        return true;
    }

    const auto temporary = fs::path(binary.wstring() + L".v2.tmp");
    FeatureBinaryHeader written;
    if (!FeatureBinaryCodec::Save(temporary, features, header.sourceXmlSha256, error, &written)) {
        std::cerr << "  cannot write " << temporary.string() << ": " << error << '\n';
        fs::remove(temporary);
        ++totals.failed;
        return false;
    }

    ImageFeatureData restored;
    FeatureBinaryHeader restoredHeader;
    if (!FeatureBinaryCodec::Load(temporary, restored, error, &restoredHeader) ||
        !Acceptable(features, restored, FeatureBinaryCodec::QuantizationTolerance(restoredHeader), error)) {
        std::cerr << "  verification failed for " << binary.string() << ": " << error << '\n';
        fs::remove(temporary);
        ++totals.failed;
        return false;
    }

    const auto bytesAfter = fs::file_size(temporary, sizeError);
    if (sizeError) {
        std::cerr << "  cannot size " << temporary.string() << '\n';
        fs::remove(temporary);
        ++totals.failed;
        return false;
    }

    const auto percent = bytesBefore > 0
        ? 100.0 * static_cast<double>(bytesAfter) / static_cast<double>(bytesBefore) : 0.0;
    std::cout << "  " << binary.filename().string() << ": " << header.keypointCount << " keypoints  "
              << (bytesBefore / 1024) << " KB -> " << (bytesAfter / 1024) << " KB  ("
              << static_cast<int>(percent) << "%)\n";

    totals.keypoints += header.keypointCount;
    totals.bytesBefore += bytesBefore;
    totals.bytesAfter += bytesAfter;

    if (!apply) {
        fs::remove(temporary);
        return true;
    }
    if (!AtomicReplace(temporary, binary)) {
        std::cerr << "  cannot replace " << binary.string() << '\n';
        fs::remove(temporary);
        ++totals.failed;
        return false;
    }
    UpdateSidecar(binary, restoredHeader, error);
    if (!error.empty()) std::cerr << "  sidecar note: " << error << '\n';
    return true;
}

}  // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2) {
        std::wcerr << L"Usage: IMaoFeatureMigrate <root> [<root> ...] [--apply]\n"
                      L"Rewrites every IMAOFT01 v1 binary under each root as v2, in place.\n"
                      L"Without --apply it reports what the rewrite would cost and changes nothing.\n";
        return 2;
    }
    std::vector<fs::path> roots;
    bool apply = false;
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring argument(arguments[index]);
        if (argument == L"--apply") { apply = true; continue; }
        roots.emplace_back(argument);
    }
    if (roots.empty()) {
        std::wcerr << L"No root directory was given.\n";
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    Totals totals;
    for (const auto& root : roots) {
        if (!fs::is_directory(root)) {
            std::wcerr << L"Not a directory: " << root.wstring() << L"\n";
            return 2;
        }
        std::cout << "== " << root.string() << (apply ? " (applying)" : " (dry run)") << '\n';
        std::vector<fs::path> binaries;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == L".imf") {
                binaries.push_back(entry.path());
            }
        }
        std::sort(binaries.begin(), binaries.end());
        for (const auto& binary : binaries) {
            ++totals.files;
            MigrateFile(binary, totals, apply);
        }
    }

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto saved = totals.bytesBefore > totals.bytesAfter
        ? totals.bytesBefore - totals.bytesAfter : 0;
    std::cout << "\n" << (apply ? "migrated" : "would migrate") << " " << totals.files << " files: "
              << totals.skipped << " already v2, " << totals.failed << " failed\n"
              << "  keypoints       " << totals.keypoints << "\n"
              << "  before          " << (totals.bytesBefore / 1048576) << " MB\n"
              << "  after           " << (totals.bytesAfter / 1048576) << " MB\n"
              << "  saved           " << (saved / 1048576) << " MB\n"
              << "  elapsed         " << static_cast<int>(seconds) << " s\n";
    return totals.failed == 0 ? 0 : 1;
}
