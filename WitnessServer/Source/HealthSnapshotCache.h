#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

// Only immutable serialized summaries are retained. No requests, sessions,
// credentials, raw work samples or incident rings belong in this cache.
// Start/Stop have one lifecycle owner; Read is safe from concurrent handlers.
class HealthSnapshotCache
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MaximumBytes = 2 * 1024 * 1024;
    struct Entry { std::string Body; Clock::time_point Completed; };
    struct ReadResult
    {
        std::shared_ptr<const Entry> Value;
        uint64_t FailedCollections = 0;
        bool LastCollectionFailed = false;
    };

    ~HealthSnapshotCache() { Stop(); }
    void Start(std::function<std::string()> Collect,
               std::chrono::milliseconds Interval = std::chrono::seconds(2))
    {
        if (Interval.count() <= 0) throw std::invalid_argument("health interval");
        Stop();
        {
            std::lock_guard Lock(Mutex);
            Stopping = false;
            Latest = {};
        }
        Worker = std::thread([this, Collect = std::move(Collect), Interval]() {
            for (;;)
            {
                { std::lock_guard Lock(Mutex); if (Stopping) return; }
                try
                {
                    auto Body = Collect();
                    if (Body.size() < 2 || Body.front() != '{' || Body.back() != '}' ||
                        Body.size() > MaximumBytes)
                        throw std::runtime_error("invalid or oversized health summary");
                    auto Value = std::make_shared<const Entry>(Entry{std::move(Body), Clock::now()});
                    std::lock_guard Lock(Mutex);
                    Latest.Value = std::move(Value);
                    Latest.LastCollectionFailed = false;
                }
                catch (...)
                {
                    // Keep the last valid sample, and disclose its age/failure.
                    // Observer failure must not kill the server or its worker.
                    std::lock_guard Lock(Mutex);
                    ++Latest.FailedCollections;
                    Latest.LastCollectionFailed = true;
                }
                std::unique_lock Lock(Mutex);
                if (Wake.wait_for(Lock, Interval, [this] { return Stopping; })) return;
            }
        });
    }
    void Stop()
    {
        { std::lock_guard Lock(Mutex); Stopping = true; }
        Wake.notify_all();
        if (Worker.joinable()) Worker.join();
        std::lock_guard Lock(Mutex);
        Latest = {};
    }
    ReadResult Read() const
    {
        std::lock_guard Lock(Mutex);
        return Latest;
    }
private:
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::thread Worker;
    bool Stopping = true;
    ReadResult Latest;
};
