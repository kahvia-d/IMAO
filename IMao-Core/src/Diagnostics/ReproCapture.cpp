#include "ReproCapture.h"
#include <opencv2/imgcodecs.hpp>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <Windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")

namespace ReproCapture {
namespace {
using Clock = std::chrono::steady_clock;
thread_local Json context = Json::object();
// Kept outside user-mergeable JSON: an old asynchronous scope must never
// manufacture evidence for a newly started recording.
thread_local std::string scopeRecording;
std::string utf8(const std::filesystem::path& p) { auto s = p.u8string(); return {s.begin(), s.end()}; }
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm{}; gmtime_s(&tm, &seconds);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
    std::ostringstream s; s << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(6) << std::setfill('0') << micros << 'Z'; return s.str();
}
struct Item { std::string record, imagePath; cv::Mat image; size_t reserved = 0; bool xpress = false; };
class Recorder {
public:
    std::mutex mutex, lifecycle;
    std::condition_variable cv;
    std::thread worker;
    std::deque<Item> queue;
    Options options;
    std::filesystem::path directory;
    Clock::time_point began{}, finished{};
    std::string state = "idle", error;
    bool accepting = false, running = false, failed = false;
    size_t queuedBytes = 0;
    uint64_t savedImages = 0, missingImages = 0, missingRecords = 0, savedRecords = 0, nextId = 0, sequence = 0;
    std::atomic<uint64_t> diskBytes{0};
    ~Recorder() { shutdown(); }
    double elapsed() const { if(state == "idle") return 0; return std::chrono::duration<double, std::milli>(((state == "recording" || state == "draining") ? Clock::now() : finished) - began).count(); }
    Json status() const { return {{"state",state},{"directory",utf8(directory)},{"elapsedMs",static_cast<int64_t>((std::max)(0.0,elapsed()))},{"durationMs",options.duration.count()},{"savedImages",savedImages},{"savedRecords",savedRecords},{"missingImages",missingImages},{"missingRecords",missingRecords},{"queuedBytes",queuedBytes},{"diskBytes",diskBytes.load()},{"error",error}}; }
    void fail(const std::string& reason, bool image = false, uint64_t missed = 1) {
        failed = true; accepting = false; state = "draining";
        if(error.empty()) error = reason;
        missingRecords += missed; if(image) missingImages += missed;
        cv.notify_all();
    }
    void write(const std::filesystem::path& p, const char* data, size_t size) {
        if(size > options.maxDiskBytes - (std::min)(diskBytes.load(),options.maxDiskBytes)) throw std::runtime_error("session disk byte limit exceeded");
        std::error_code ec; auto available = std::filesystem::space(directory, ec);
        if(ec || available.available < size + 1024 * 1024) throw std::runtime_error("insufficient free disk space for evidence");
        std::ofstream f(p, std::ios::binary | std::ios::trunc); f.exceptions(std::ios::badbit | std::ios::failbit);
        f.write(data, static_cast<std::streamsize>(size)); f.close(); diskBytes += size;
    }
    void writeXpress(const Item& item) {
        const size_t rawBytes=item.image.total()*item.image.elemSize();
        const size_t count=(std::min)(size_t{4},rawBytes);
        struct Chunk { std::vector<uchar> bytes; SIZE_T size=0; DWORD error=ERROR_SUCCESS; };
        std::vector<Chunk> chunks(count);
        std::vector<std::jthread> encoders;
        // This fixed fan-out is part of the bounded per-item encoding allowance.
        // Each block is an independent Windows Compression API buffer.
        for(size_t i=0;i<count;++i) encoders.emplace_back([&,i] {
            COMPRESSOR_HANDLE compressor=nullptr;
            try {
                const size_t from=rawBytes*i/count, length=rawBytes*(i+1)/count-from;
                chunks[i].bytes.resize(length*2+65536);
                if(!CreateCompressor(COMPRESS_ALGORITHM_XPRESS,nullptr,&compressor)) { chunks[i].error=GetLastError(); return; }
                if(!Compress(compressor,item.image.data+from,length,chunks[i].bytes.data(),chunks[i].bytes.size(),&chunks[i].size)) chunks[i].error=GetLastError();
            } catch(...) { chunks[i].error=ERROR_NOT_ENOUGH_MEMORY; }
            if(compressor) CloseCompressor(compressor);
        });
        for(auto& encoder:encoders) encoder.join();
        uint64_t total=12+16*count;
        for(const auto& chunk:chunks) {
            if(chunk.error!=ERROR_SUCCESS) throw std::runtime_error("Windows XPRESS compression failed: "+std::to_string(chunk.error));
            total+=chunk.size;
        }
        if(total>options.maxDiskBytes-(std::min)(diskBytes.load(),options.maxDiskBytes)) throw std::runtime_error("session disk byte limit exceeded");
        std::error_code ec; const auto free=std::filesystem::space(directory,ec);
        if(ec||free.available<total+1024*1024) throw std::runtime_error("insufficient free disk space for evidence");
        std::ofstream f(directory/std::filesystem::u8path(item.imagePath),std::ios::binary);
        f.exceptions(std::ios::badbit|std::ios::failbit);
        const auto integer=[&](uint64_t n,size_t bytes) { for(size_t k=0;k<bytes;++k) f.put(static_cast<char>((n>>(8*k))&255)); };
        f.write("IMAOXPR1",8); integer(count,4);
        for(size_t i=0;i<count;++i) {
            integer(rawBytes*(i+1)/count-rawBytes*i/count,8); integer(chunks[i].size,8);
            f.write(reinterpret_cast<const char*>(chunks[i].bytes.data()),static_cast<std::streamsize>(chunks[i].size));
        }
        f.close(); diskBytes+=total;
    }
    void run() noexcept {
        try {
            std::ofstream records(directory / "records.jsonl", std::ios::binary);
            records.exceptions(std::ios::badbit | std::ios::failbit);
            for(;;) {
                Item item;
                {
                    std::unique_lock lock(mutex);
                    cv.wait_until(lock, began + options.duration, [&] { return !accepting || !queue.empty(); });
                    if(Clock::now() >= began + options.duration) { accepting = false; state = "draining"; }
                    if(queue.empty()) { if(!accepting) break; continue; }
                    item = std::move(queue.front()); queue.pop_front();
                }
                bool wrote = false;
                try {
                    if(!item.image.empty()) {
                        if(item.xpress) writeXpress(item);
                        else {
                        std::vector<uchar> encoded;
                        if(!cv::imencode(".png", item.image, encoded, {cv::IMWRITE_PNG_COMPRESSION, 1})) throw std::runtime_error("PNG encoding failed");
                        const auto path = std::filesystem::u8path(item.imagePath);
                        write(directory / path, reinterpret_cast<const char*>(encoded.data()), encoded.size());
                        }
                    }
                    const auto& line = item.record;
                    if(line.size() > options.maxDiskBytes - (std::min)(diskBytes.load(), options.maxDiskBytes)) throw std::runtime_error("session disk byte limit exceeded");
                    records.write(line.data(), static_cast<std::streamsize>(line.size())); records.flush();
                    diskBytes += line.size(); wrote = true;
                } catch(const std::exception& e) { std::lock_guard lock(mutex); fail(e.what(), !item.image.empty()); }
                {
                    std::lock_guard lock(mutex); queuedBytes -= item.reserved;
                    if(wrote) { ++savedRecords; if(!item.image.empty()) ++savedImages; }
                }
            }
            records.close();
        } catch(const std::exception& e) {
            std::lock_guard lock(mutex); fail(e.what(), false, 0);
            for(auto& item: queue) { ++missingRecords; if(!item.image.empty()) ++missingImages; }
            queue.clear(); queuedBytes = 0;
        } catch(...) {
            std::lock_guard lock(mutex); fail("unknown writer exception", false, 0);
            for(auto& item: queue) { ++missingRecords; if(!item.image.empty()) ++missingImages; }
            queue.clear(); queuedBytes = 0;
        }
        std::lock_guard lock(mutex);
        accepting = false; finished = Clock::now(); state = failed ? "failed" : "completed";
        try {
            Json completion = status(); completion["complete"] = !failed; completion["endedAt"] = timestamp();
            const auto text = completion.dump(2);
            // The manifest remains best-effort even when the evidence budget has been exhausted.
            std::ofstream f(directory / "completion.json", std::ios::binary); f.exceptions(std::ios::badbit | std::ios::failbit); f << text; f.close();
        } catch(const std::exception& e) { failed = true; state = "failed"; if(error.empty()) error = std::string("completion manifest: ") + e.what(); }
        running = false; cv.notify_all();
    }
    void shutdown() noexcept {
        try { std::lock_guard life(lifecycle); { std::lock_guard lock(mutex); accepting = false; if(running) state = "draining"; cv.notify_all(); } if(worker.joinable()) worker.join(); } catch(...) {}
    }
};
Recorder& recorder() { static Recorder r; return r; }
void enqueue(const std::string& kind, const std::string& stage, const cv::Mat& image, Json metadata) noexcept {
    auto& r = recorder();
    try {
        std::lock_guard lock(r.mutex);
        if(!r.accepting) return;
        if(!scopeRecording.empty() && scopeRecording != utf8(r.directory)) return;
        if(Clock::now() >= r.began + r.options.duration) { r.accepting = false; r.state = "draining"; r.cv.notify_all(); return; }
        try {
            if(kind == "image" && (image.empty() || image.depth() != CV_8U || (image.channels()!=1 && image.channels()!=3 && image.channels()!=4))) throw std::runtime_error("image must be nonempty 8-bit gray/BGR/BGRA for lossless PNG");
            Json record = {{"recordId", ++r.nextId},{"recordingId",utf8(r.directory)},{"kind",kind},{"stage",stage},{"timestamp",timestamp()},{"steadyMs",r.elapsed()},{"context",context},{"metadata",std::move(metadata)}};
            const bool xpress=kind=="image"&&stage=="wgc";
            if(kind == "image") record["image"] = {{"path", "images/" + std::to_string(r.nextId) + (xpress?".xpress":".png")},{"width",image.cols},{"height",image.rows},{"type",image.type()},{"channels",image.channels()},
                {"encoding",xpress?"windows-xpress-chunks-v1":"png"},{"rawBytes",image.total()*image.elemSize()},{"rowBytes",static_cast<size_t>(image.cols)*image.elemSize()}};
            auto serialized = record.dump(-1,' ',false,Json::error_handler_t::replace) + '\n';
            const auto jsonBytes = serialized.capacity();
            const auto raw = image.total() * image.elemSize();
            // Charge both the owned raw pixels and a conservative PNG output allowance,
            // plus serialized JSON/node overhead, until the writer finishes the item.
            if(raw > ((std::numeric_limits<size_t>::max)() - 262144) / 3 || jsonBytes > ((std::numeric_limits<size_t>::max)() - 1024) / 4) throw std::runtime_error("record size overflow");
            const size_t reserved = jsonBytes * 4 + 1024 + (raw ? raw * 3 + 262144 : 0);
            if(reserved > r.options.maxQueuedBytes || r.queuedBytes > r.options.maxQueuedBytes - reserved) { r.fail("bounded evidence queue capacity exceeded", kind == "image"); return; }
            r.queuedBytes += reserved; // Reserve before cloning, including the in-flight encoder.
            try { r.queue.push_back(Item{std::move(serialized), kind == "image" ? record["image"]["path"].get<std::string>() : std::string{}, image.empty() ? cv::Mat{} : image.clone(), reserved, xpress}); }
            catch(...) { r.queuedBytes -= reserved; throw; }
            r.cv.notify_one();
        } catch(const std::exception& e) { r.fail(e.what(),kind == "image"); }
          catch(...) { r.fail("unknown evidence enqueue exception",kind == "image"); }
    } catch(...) {}
}
}
bool Start(const std::filesystem::path& parent, const Json& metadata, const Options& options) {
    auto& r = recorder();
    try {
        std::lock_guard life(r.lifecycle); std::unique_lock lock(r.mutex);
        if(r.running) return false;
        if(r.worker.joinable()) r.worker.join();
        r.options = options; r.directory.clear(); r.error.clear(); r.queue.clear();
        r.accepting = false; r.failed = false; r.queuedBytes = 0; r.savedImages = r.missingImages = r.missingRecords = r.savedRecords = r.nextId = 0; r.diskBytes = 0;
        r.began = r.finished = Clock::now(); r.state = "idle";
        try {
            if(options.duration.count() <= 0 || options.maxQueuedBytes == 0 || options.maxDiskBytes < 1024) throw std::runtime_error("invalid recorder options");
            std::filesystem::create_directories(parent);
            auto name = timestamp(); for(auto& c:name) if(c == ':' || c == '.') c = '-';
            for(;;) { r.directory = parent / ("repro-" + name + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++r.sequence)); if(std::filesystem::create_directory(r.directory)) break; }
            std::filesystem::create_directory(r.directory / "images");
            auto free = std::filesystem::space(r.directory); if(free.available < 64ull*1024*1024) throw std::runtime_error("low free disk space (less than 64 MiB)");
            Json manifest = {{"startedAt",timestamp()},{"durationMs",options.duration.count()},{"maxQueuedBytes",options.maxQueuedBytes},{"maxDiskBytes",options.maxDiskBytes},{"metadata",metadata},{"schemaVersion",1}};
            auto text = manifest.dump(2,' ',false,Json::error_handler_t::replace); r.write(r.directory / "metadata.json", text.data(), text.size());
            r.accepting = r.running = true; r.state = "recording";
            r.worker = std::thread([&r] { r.run(); }); return true;
        } catch(const std::exception& e) {
            r.error=e.what(); r.failed=true; r.running=r.accepting=false; r.state="failed"; r.finished=Clock::now();
            try {
                if(!r.directory.empty() && std::filesystem::is_directory(r.directory)) {
                    auto completion = r.status(); completion["complete"] = false;
                    std::ofstream f(r.directory / "completion.json", std::ios::binary);
                    f.exceptions(std::ios::badbit | std::ios::failbit); f << completion.dump(2); f.close();
                }
            } catch(...) {} // Status still exposes the original preflight error if storage is unavailable.
            return false;
        }
    } catch(...) { return false; }
}
void Stop() { try { auto& r=recorder(); std::lock_guard lock(r.mutex); if(r.running) { r.accepting=false; r.state="draining"; r.cv.notify_all(); } } catch(...) {} }
void Shutdown() { recorder().shutdown(); }
bool Active() { try { auto& r=recorder(); std::lock_guard lock(r.mutex); return r.accepting && Clock::now() < r.began+r.options.duration; } catch(...) { return false; } }
Json Status() { auto& r=recorder(); std::lock_guard lock(r.mutex); return r.status(); }
Scope::Scope(Json values) : previous_(context), previousRecording_(scopeRecording) {
    auto merged=context; if(values.is_object()) merged.update(values);
    if(scopeRecording.empty()) { auto& r=recorder(); std::lock_guard lock(r.mutex); if(r.accepting) scopeRecording=utf8(r.directory); }
    context.swap(merged);
}
Scope::~Scope() { context.swap(previous_); scopeRecording.swap(previousRecording_); }
Json Context() { return context; }
void Image(const std::string& stage, const cv::Mat& image, Json metadata) { enqueue("image",stage,image,std::move(metadata)); }
void Record(const std::string& stage, Json metadata) { enqueue("stage",stage,{},std::move(metadata)); }
void Log(Json event) { enqueue("log","log",{},std::move(event)); }
}
