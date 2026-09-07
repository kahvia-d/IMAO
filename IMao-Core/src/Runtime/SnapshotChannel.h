#pragma once
#include <atomic>
#include <memory>

// A publication replaces the entire value. Consumers keep a consistent old frame
// until they finish, even while the producer publishes the next one.
template<class T> class SnapshotChannel {
public:
    std::shared_ptr<const T> Read() const { return value_.load(std::memory_order_acquire); }
    void Publish(T value) { value_.store(std::make_shared<const T>(std::move(value)), std::memory_order_release); }
private:
    std::atomic<std::shared_ptr<const T>> value_{std::make_shared<const T>()};
};
