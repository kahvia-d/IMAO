// Rewrites a feature binary's sidecar manifest from the binary's own header.
//
// The sidecar is the machine-readable record of what the binary contains: format,
// version, descriptor encoding, keypoint counts and the payload hash. Deriving it
// from the file rather than patching it in place means the record cannot drift from
// the binary it describes - which is what happened when the migration updated only
// the version and payload hash and left the quantization fields out.
#include "Feature/Processing/FeatureBinaryCodec.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

bool AtomicReplace(const fs::path& temporary, const fs::path& destination) {
    return MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

/// Reads one fixed-width little-endian integer field.
template <typename T>
bool ReadField(const std::vector<std::uint8_t>& bytes, std::size_t& offset, T& output) {
    if (offset + sizeof(T) > bytes.size()) return false;
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<Unsigned>(bytes[offset++]) << (index * 8);
    }
    output = static_cast<T>(value);
    return true;
}

/// Reads one float field, whose bit pattern is reinterpreted the way the codec writes it.
bool ReadFloatField(const std::vector<std::uint8_t>& bytes, std::size_t& offset, float& output) {
    std::uint32_t bits = 0;
    if (!ReadField(bytes, offset, bits)) return false;
    std::memcpy(&output, &bits, sizeof(output));
    return true;
}

std::string Number(float value) {
    std::ostringstream text;
    text << std::setprecision(9) << value;
    return text.str();
}

/// Reads the header straight out of the file. A sidecar only describes the header, so
/// inflating every descriptor to produce it would be wasted work on a 20 MB pack.
bool ReadHeader(const fs::path& binary, FeatureBinaryHeader& header, std::string& error) {
    std::ifstream input(binary, std::ios::binary);
    if (!input) {
        error = "cannot open";
        return false;
    }
    std::vector<std::uint8_t> bytes(FeatureBinaryHeader::QuantizedSerializedSize);
    const auto fixed = static_cast<std::size_t>(FeatureBinaryHeader::SerializedSize);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fixed));
    if (input.gcount() != static_cast<std::streamsize>(fixed)) {
        error = "header is truncated";
        return false;
    }
    if (!std::equal(FeatureBinaryHeader::Magic.begin(), FeatureBinaryHeader::Magic.end(), bytes.begin())) {
        error = "not an IMAOFT01 binary";
        return false;
    }
    std::size_t offset = FeatureBinaryHeader::Magic.size();
    if (!ReadField(bytes, offset, header.version) ||
        !ReadField(bytes, offset, header.headerLength) ||
        !ReadField(bytes, offset, header.endianMarker) ||
        !ReadField(bytes, offset, header.keypointCount) ||
        !ReadField(bytes, offset, header.descriptorRows) ||
        !ReadField(bytes, offset, header.descriptorColumns) ||
        !ReadField(bytes, offset, header.descriptorType) ||
        !ReadField(bytes, offset, header.keypointPayloadLength) ||
        !ReadField(bytes, offset, header.descriptorPayloadLength)) {
        error = "header is unreadable";
        return false;
    }
    if (header.version >= FeatureBinaryHeader::CurrentVersion) {
        const auto remaining = static_cast<std::size_t>(header.headerLength) - fixed;
        input.read(reinterpret_cast<char*>(bytes.data() + fixed), static_cast<std::streamsize>(remaining));
        if (input.gcount() != static_cast<std::streamsize>(remaining)) {
            error = "quantized header is truncated";
            return false;
        }
        if (!ReadFloatField(bytes, offset, header.descriptorScale) ||
            !ReadFloatField(bytes, offset, header.descriptorMaximum) ||
            !ReadFloatField(bytes, offset, header.descriptorMinimum) ||
            !ReadField(bytes, offset, header.descriptorOffset)) {
            error = "quantized header is unreadable";
            return false;
        }
    }
    const auto hashStart = static_cast<std::size_t>(header.headerLength) -
        header.sourceXmlSha256.size() - header.payloadSha256.size();
    if (hashStart < offset || hashStart + 64 > bytes.size()) {
        error = "header hash offset is invalid";
        return false;
    }
    std::copy_n(bytes.begin() + hashStart, header.sourceXmlSha256.size(), header.sourceXmlSha256.begin());
    std::copy_n(bytes.begin() + hashStart + header.sourceXmlSha256.size(),
        header.payloadSha256.size(), header.payloadSha256.begin());
    return true;
}

/// Writes the same field set the converter writes for a freshly built binary, so a
/// migrated pack and a rebuilt one are indistinguishable to the release checks.
std::string SerializeSidecar(const FeatureBinaryHeader& header) {
    const bool quantized = header.version >= FeatureBinaryHeader::CurrentVersion;
    std::ostringstream json;
    json << "{\n";
    json << "  \"descriptorColumns\": " << header.descriptorColumns << ",\n";
    json << "  \"descriptorRows\": " << header.descriptorRows << ",\n";
    json << "  \"descriptorType\": \"" << (quantized ? "quantized-uint8" : "float32") << "\",\n";
    json << "  \"deflated\": " << (quantized ? "true" : "false") << ",\n";
    json << "  \"format\": \"IMAOFT01\",\n";
    if (quantized) {
        json << "  \"descriptorMaximum\": " << Number(header.descriptorMaximum) << ",\n";
        json << "  \"descriptorMinimum\": " << Number(header.descriptorMinimum) << ",\n";
        json << "  \"descriptorOffset\": " << header.descriptorOffset << ",\n";
        json << "  \"descriptorScale\": " << Number(header.descriptorScale) << ",\n";
        json << "  \"quantizationStep\": " << Number(FeatureBinaryCodec::QuantizationTolerance(header)) << ",\n";
    }
    json << "  \"keypointCount\": " << header.keypointCount << ",\n";
    json << "  \"payloadSha256\": \"" << FeatureBinaryCodec::Sha256Hex(header.payloadSha256) << "\",\n";
    json << "  \"sourceXmlSha256\": \"" << FeatureBinaryCodec::Sha256Hex(header.sourceXmlSha256) << "\",\n";
    json << "  \"version\": " << header.version << "\n";
    json << "}\n";
    return json.str();
}

}  // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2) {
        std::wcerr << L"Usage: IMaoFeatureSidecar <root> [<root> ...] [--apply]\n"
                      L"Rewrites every feature binary's .manifest.json from the binary's own header.\n"
                      L"Without --apply it reports what would change and writes nothing.\n";
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

    std::uint64_t written = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t failed = 0;
    for (const auto& root : roots) {
        if (!fs::is_directory(root)) {
            std::wcerr << L"Not a directory: " << root.wstring() << L"\n";
            return 2;
        }
        std::vector<fs::path> binaries;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == L".imf") {
                binaries.push_back(entry.path());
            }
        }
        std::sort(binaries.begin(), binaries.end());
        for (const auto& binary : binaries) {
            FeatureBinaryHeader header;
            std::string error;
            if (!ReadHeader(binary, header, error)) {
                std::cerr << "  cannot read " << binary.string() << ": " << error << '\n';
                ++failed;
                continue;
            }
            const auto text = SerializeSidecar(header);
            const auto sidecar = fs::path(binary.wstring() + L".manifest.json");
            std::string existing;
            if (fs::exists(sidecar)) {
                std::ifstream current(sidecar);
                existing.assign(std::istreambuf_iterator<char>(current), std::istreambuf_iterator<char>());
            }
            if (existing == text) {
                ++unchanged;
                continue;
            }
            if (apply) {
                const auto temporary = fs::path(sidecar.wstring() + L".tmp");
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << text;
                output.close();
                if (!output || !AtomicReplace(temporary, sidecar)) {
                    std::cerr << "  cannot write " << sidecar.string() << '\n';
                    fs::remove(temporary);
                    ++failed;
                    continue;
                }
            }
            std::cout << "  " << (apply ? "wrote  " : "would write ")
                      << binary.parent_path().filename().string() << "/" << binary.filename().string()
                      << "  version=" << header.version << " keypoints=" << header.keypointCount << '\n';
            ++written;
        }
    }

    std::cout << "\n" << (apply ? "rewrote" : "would rewrite") << " " << written
              << " sidecars (" << unchanged << " already current, " << failed << " failed)\n";
    return failed == 0 ? 0 : 1;
}
