#pragma once
#include <chrono>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

// Bounded camera I/O, independent of Crow. Ordered control lanes per physical
// camera, separate from polling; a reserved worker handles Stop requests.
class PtzRequestExecutor
{
public:
    struct Completions
    {
        void Add() { std::lock_guard lock(Mutex); ++Pending; }
        void Done() { { std::lock_guard lock(Mutex); --Pending; } Wake.notify_all(); }
        bool Drain() { std::unique_lock lock(Mutex); return Wake.wait_for(lock, std::chrono::seconds(5), [&] { return Pending == 0; }); }
    private:
        std::mutex Mutex;
        std::condition_variable Wake;
        size_t Pending = 0;
    };
    enum class Kind { Read, Write, Movement, Stop };
    struct Job
    {
        int Lane;
        Kind Type;
        std::function<void()> Run;
        std::function<void()> Cancel;
        std::chrono::steady_clock::time_point Queued = std::chrono::steady_clock::now();
    };
    ~PtzRequestExecutor() { Stop(); }
    void Start();
    // False means admission failed; ownership remains with the caller.
    bool Submit(Job job);
    void Stop();
    // Checked immediately before motor dispatch after potentially slow
    // discovery. Stop invalidates an active gesture that has not sent yet.
    static bool MovementCancelled();
private:
    void Worker(bool StopsOnly);
    std::mutex Mutex;
    std::condition_variable Wake;
    std::deque<Job> Queue;
    std::unordered_set<int> Active;
    std::unordered_set<int> ActiveReads;
    std::unordered_map<int, std::shared_ptr<std::atomic<bool>>> ActiveMovements;
    std::vector<std::thread> Workers;
    bool Closing = true;
};
