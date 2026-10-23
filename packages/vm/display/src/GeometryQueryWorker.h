#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

// One owned query and one completion slot. Only the transport thread calls
// start/take; stop is called after that transport thread has finished its loop.
class GeometryQueryWorker {
public:
    struct Result {
        std::uint64_t generation = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool success = false;
    };

    explicit GeometryQueryWorker(std::function<void()> cancel)
        : cancel_(std::move(cancel))
    {
    }

    ~GeometryQueryWorker()
    {
        stop();
    }

    bool start(std::uint64_t generation, std::function<Result()> query)
    {
        if (stopped_ || worker_.joinable())
            return false;
        complete_ = false;
        worker_ = std::thread([this, generation, query = std::move(query)] {
            auto result = query();
            result.generation = generation;
            {
                std::lock_guard lock(mutex_);
                result_ = result;
            }
            complete_ = true;
        });
        return true;
    }

    std::optional<Result> take()
    {
        if (!complete_)
            return std::nullopt;
        worker_.join();
        complete_ = false;
        std::lock_guard lock(mutex_);
        return result_;
    }

    void stop()
    {
        if (stopped_)
            return;
        stopped_ = true;
        cancel_();
        if (worker_.joinable())
            worker_.join();
        complete_ = false;
    }

private:
    std::function<void()> cancel_;
    std::thread worker_;
    std::atomic<bool> complete_{false};
    bool stopped_ = false;
    std::mutex mutex_;
    Result result_;
};
