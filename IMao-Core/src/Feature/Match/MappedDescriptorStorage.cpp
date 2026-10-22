#include "MappedDescriptorStorage.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <limits>
#include <new>

namespace MappedDescriptorStorage {
namespace {
std::atomic_uint64_t liveBytes{0}, liveMappings{0}, failures{0}, closeErrors{0};
void CloseFailure() noexcept {
    closeErrors.fetch_add(1, std::memory_order_relaxed);
    OutputDebugStringA("IMao mapped descriptor cleanup failed; descriptor-storage closeErrors is incomplete\n");
}
struct Mapping {
    HANDLE file = INVALID_HANDLE_VALUE, section = nullptr;
    void* view = nullptr;
    size_t bytes = 0;
    bool counted = false;
    ~Mapping() {
        bool closed = true;
        if (view && !UnmapViewOfFile(view)) { CloseFailure(); closed = false; }
        if (section && !CloseHandle(section)) { CloseFailure(); closed = false; }
        if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) { CloseFailure(); closed = false; }
        if (counted && closed) { liveBytes.fetch_sub(bytes); liveMappings.fetch_sub(1); }
    }
};
class Allocator final : public cv::MatAllocator {
public:
    cv::UMatData* allocate(int dims, const int* sizes, int type, void* data, size_t* step,
        cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        return cv::Mat::getDefaultAllocator()->allocate(dims, sizes, type, data, step, flags, usage);
    }
    bool allocate(cv::UMatData* data, cv::AccessFlag flags, cv::UMatUsageFlags usage) const override {
        return cv::Mat::getDefaultAllocator()->allocate(data, flags, usage);
    }
    void deallocate(cv::UMatData* data) const override {
        delete static_cast<Mapping*>(data->userdata);
        delete data;
    }
};
Allocator& StorageAllocator() {
    // Mat headers can survive other static owners at shutdown. Intentionally
    // keep the allocator object alive for the entire process, without a destructor.
    alignas(Allocator) static unsigned char storage[sizeof(Allocator)];
    static Allocator* allocator = new (storage) Allocator;
    return *allocator;
}
std::string WinError(const char* operation) { return std::string(operation) + " Win32=" + std::to_string(GetLastError()); }
bool Write(HANDLE file, const unsigned char* data, size_t bytes, std::string& error) {
    while (bytes) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr) || written != chunk) { error = WinError("descriptor write"); return false; }
        data += written; bytes -= written;
    }
    return true;
}
bool Build(const cv::Mat& source, cv::Mat& output, std::string& error, const std::filesystem::path& directory) {
    if (source.empty() || source.dims != 2 || (source.type() != CV_8UC1 && source.type() != CV_32FC1)) {
        error = "descriptor mapping requires nonempty 2D uint8/float32 rows"; return false;
    }
    std::filesystem::path root = directory;
    if (root.empty()) {
        wchar_t buffer[MAX_PATH + 1]{};
        const DWORD count = GetTempPathW(MAX_PATH + 1, buffer);
        if (!count || count > MAX_PATH) { error = WinError("descriptor temporary directory"); return false; }
        root = buffer;
    }
    std::array<unsigned char, 16> nonce{};
    const NTSTATUS randomStatus = BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (randomStatus != 0) { error = "descriptor temporary name RNG status=" + std::to_string(static_cast<long>(randomStatus)); return false; }
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"imd-";
    for (auto value : nonce) { name += hex[value >> 4]; name += hex[value & 15]; }
    name += L".tmp";
    const auto filename = root / name;
    if (filename.native().size() >= MAX_PATH) { error = "descriptor temporary path is too long"; return false; }
    auto owner = std::make_unique<Mapping>();
    // Atomic creation with DELETE_ON_CLOSE: even abrupt process exit cannot
    // leave a file created before a later ownership/open step.
    owner->file = CreateFileW(filename.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (owner->file == INVALID_HANDLE_VALUE) { error = WinError("descriptor file create"); return false; }
    const size_t rowBytes = source.cols * source.elemSize();
    owner->bytes = source.total() * source.elemSize();
    if (source.isContinuous()) {
        if (!Write(owner->file, source.data, owner->bytes, error)) return false;
    } else {
        for (int row = 0; row < source.rows; ++row)
            if (!Write(owner->file, source.ptr(row), rowBytes, error)) return false;
    }
    if (!FlushFileBuffers(owner->file)) { error = WinError("descriptor flush"); return false; }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(owner->file, beginning, nullptr, FILE_BEGIN)) { error = WinError("descriptor verify seek"); return false; }
    // Verify every byte before exposing a read-only view. No resource-file hash
    // or global row identity changes; noncontinuous inputs are packed row-wise.
    std::vector<unsigned char> check(std::min<size_t>(owner->bytes, 1024 * 1024));
    size_t offset = 0;
    while (offset < owner->bytes) {
        const DWORD chunk = static_cast<DWORD>(std::min(check.size(), owner->bytes - offset));
        DWORD received = 0;
        if (!ReadFile(owner->file, check.data(), chunk, &received, nullptr) || received != chunk) { error = WinError("descriptor verify read"); return false; }
        size_t compared = 0;
        while (compared < chunk) {
            const size_t at = offset + compared, row = at / rowBytes, column = at % rowBytes;
            const size_t count = std::min<size_t>(chunk - compared, rowBytes - column);
            if (std::memcmp(source.ptr(static_cast<int>(row)) + column, check.data() + compared, count)) {
                error = "descriptor temporary file verification mismatch"; return false;
            }
            compared += count;
        }
        offset += chunk;
    }
    owner->section = CreateFileMappingW(owner->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!owner->section) { error = WinError("descriptor file mapping"); return false; }
    owner->view = MapViewOfFile(owner->section, FILE_MAP_READ, 0, 0, 0);
    if (!owner->view) { error = WinError("descriptor read-only view"); return false; }
    cv::Mat mapped(source.rows, source.cols, source.type(), owner->view);
    auto data = std::make_unique<cv::UMatData>(&StorageAllocator());
    data->data = data->origdata = static_cast<unsigned char*>(owner->view);
    data->size = owner->bytes;
    data->flags = cv::UMatData::USER_ALLOCATED;
    data->refcount = 1;
    owner->counted = true;
    liveBytes.fetch_add(owner->bytes); liveMappings.fetch_add(1);
    data->userdata = owner.release();
    mapped.u = data.release();
    mapped.allocator = &StorageAllocator();
    output = std::move(mapped);
    return true;
}
}
bool Create(const cv::Mat& source, cv::Mat& output, std::string& error, const std::filesystem::path& directory) {
    error.clear();
    try {
        if (Build(source, output, error, directory)) return true;
    } catch (const std::exception& e) { error = std::string("descriptor mapping: ") + e.what(); }
    failures.fetch_add(1);
    return false;
}
bool IsMapped(const cv::Mat& image) noexcept { return image.u && image.u->currAllocator == &StorageAllocator(); }
bool ResidentBytes(const cv::Mat& image, size_t& bytes, std::string& error) {
    bytes = 0; error.clear();
    if (!IsMapped(image)) { error = "descriptor matrix is not mapped"; return false; }
    try {
        const auto* owner = static_cast<const Mapping*>(image.u->userdata);
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        const size_t pages = (owner->bytes + info.dwPageSize - 1) / info.dwPageSize;
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> entries(std::min<size_t>(pages, 4096));
        for (size_t first = 0; first < pages; first += entries.size()) {
            const size_t count = std::min(entries.size(), pages - first);
            for (size_t i = 0; i < count; ++i) {
                entries[i] = {}; entries[i].VirtualAddress = static_cast<unsigned char*>(owner->view) + (first + i) * info.dwPageSize;
            }
            if (!QueryWorkingSetEx(GetCurrentProcess(), entries.data(), static_cast<DWORD>(count * sizeof(entries[0])))) {
                error = WinError("descriptor resident query"); return false;
            }
            for (size_t i = 0; i < count; ++i) if (entries[i].VirtualAttributes.Valid) bytes += info.dwPageSize;
        }
        return true;
    } catch (const std::exception& e) { error = std::string("descriptor resident query: ") + e.what(); return false; }
}
Statistics Read() noexcept { return {liveBytes.load(), liveMappings.load(), failures.load(), closeErrors.load()}; }
}
