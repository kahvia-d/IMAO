#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <compressapi.h>
#include <array>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <limits>
#include <stdexcept>

namespace ReproCapture {
// Only one item is encoded at a time. Queue entries own pixels; the encoding
// allowance is shared, not charged again for every waiting image.
struct EvidenceReservation {
    size_t owned = 0, scratch = 0;
    static EvidenceReservation For(size_t raw, size_t json) {
        const auto limit = (std::numeric_limits<size_t>::max)();
        if (raw > (limit - 262144) / 3 || json > (limit - 1024) / 4)
            throw std::runtime_error("record size overflow");
        const size_t metadata = json * 4 + 1024;
        if (raw > limit - metadata) throw std::runtime_error("record size overflow");
        return {raw + metadata, raw ? raw * 2 + 262144 : 0};
    }
    bool Fits(size_t queued, size_t previousScratch, size_t budget) const {
        const auto workspace = (std::max)(scratch, previousScratch);
        return workspace <= budget && owned <= budget - workspace && queued <= budget - workspace - owned;
    }
};

// One session owns four reusable workers/OS compression handles. File format
// remains four independent XPRESS blocks, identical to the previous encoder.
class XpressWorkers {
public:
    XpressWorkers() {
        try {
            for (size_t i=0;i<threads.size();++i) {
                threads[i]=std::thread([this,i]{ work(i); });
                if(!SetThreadPriority(threads[i].native_handle(),THREAD_PRIORITY_LOWEST))
                    throw std::runtime_error("recording worker priority failed: "+std::to_string(GetLastError()));
            }
        }
        catch (...) { stop(); throw; }
    }
    ~XpressWorkers() { stop(); }
    void Run(std::function<void(size_t, COMPRESSOR_HANDLE)> task) {
        std::unique_lock lock(mutex);
        job=std::move(task); remaining=threads.size(); ++generation;
        wake.notify_all(); done.wait(lock,[&]{return remaining==0;}); job={};
    }
private:
    void stop() {
        { std::lock_guard lock(mutex); stopping=true; }
        wake.notify_all(); for(auto& thread:threads) if(thread.joinable())thread.join();
    }
    void work(size_t i) noexcept {
        COMPRESSOR_HANDLE compressor=nullptr;
        CreateCompressor(COMPRESS_ALGORITHM_XPRESS,nullptr,&compressor);
        size_t seen=0;
        std::unique_lock lock(mutex);
        for(;;) {
            wake.wait(lock,[&]{return stopping||generation!=seen;});
            if(stopping)break;
            seen=generation;
            lock.unlock(); job(i,compressor); lock.lock();
            if(--remaining==0)done.notify_one();
        }
        lock.unlock(); if(compressor)CloseCompressor(compressor);
    }
    std::array<std::thread,4> threads;
    std::mutex mutex;
    std::condition_variable wake,done;
    std::function<void(size_t,COMPRESSOR_HANDLE)> job;
    size_t generation=0,remaining=0;
    bool stopping=false;
};
}
