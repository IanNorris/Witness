#include "PtzRequestExecutor.h"
#include <algorithm>

namespace {
thread_local std::shared_ptr<std::atomic<bool>> CurrentCancellation;
void Call(const std::function<void()>& action) noexcept
{
    try { if (action) action(); } catch (...) {}
}
}

bool PtzRequestExecutor::MovementCancelled()
{
    return CurrentCancellation && CurrentCancellation->load();
}

void PtzRequestExecutor::Start()
{
    Stop();
    { std::lock_guard lock(Mutex); Closing = false; }
    try {
        for (int i = 0; i < 3; ++i) Workers.emplace_back([this] { Worker(false); });
        Workers.emplace_back([this] { Worker(true); });
    } catch (...) { Stop(); throw; }
}

bool PtzRequestExecutor::Submit(Job job)
{
    std::vector<Job> Cancelled;
    {
        std::lock_guard lock(Mutex);
        if (Closing || !job.Run || !job.Cancel || job.Lane < 0) return false;
        const auto Stops = std::count_if(Queue.begin(), Queue.end(),
            [](const Job& value) { return value.Type == Kind::Stop; });
        // Reserve capacity as well as a worker for safety commands.
        if (job.Type == Kind::Stop ? Stops >= 16 : Queue.size() - Stops >= 32) return false;
        if (job.Type == Kind::Stop) {
            const auto active = ActiveMovements.find(job.Lane);
            if (active != ActiveMovements.end()) active->second->store(true);
            for (auto it = Queue.begin(); it != Queue.end();) {
                if (it->Lane == job.Lane && it->Type == Kind::Movement) {
                    Cancelled.push_back(std::move(*it)); it = Queue.erase(it);
                } else ++it;
            }
            Queue.push_front(std::move(job));
        } else Queue.push_back(std::move(job));
    }
    for (const auto& value : Cancelled) Call(value.Cancel);
    Wake.notify_all();
    return true;
}

void PtzRequestExecutor::Worker(bool StopsOnly)
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(Mutex);
            auto Find = [&] {
                // Prefer Stops, even on ordinary workers. Never overlap a Stop
                // with an in-flight movement/discovery on the same device.
                auto it = std::find_if(Queue.begin(), Queue.end(), [&](const Job& value) {
                    return value.Type == Kind::Stop && !Active.count(value.Lane);
                });
                if (it != Queue.end() || StopsOnly) return it;
                return std::find_if(Queue.begin(), Queue.end(), [&](const Job& value) {
                    return value.Type == Kind::Read ? !ActiveReads.count(value.Lane) : !Active.count(value.Lane);
                });
            };
            Wake.wait(lock, [&] { return Closing || Find() != Queue.end(); });
            if (Closing) return;
            const auto it = Find();
            job = std::move(*it); Queue.erase(it);
            if (job.Type == Kind::Read) ActiveReads.insert(job.Lane); else Active.insert(job.Lane);
            if (job.Type == Kind::Movement) {
                CurrentCancellation = std::make_shared<std::atomic<bool>>(false);
                ActiveMovements[job.Lane] = CurrentCancellation;
            }
        }
        // Do not replay a stale gesture after another slow request completes.
        if (job.Type == Kind::Movement &&
            std::chrono::steady_clock::now() - job.Queued > std::chrono::seconds(2)) Call(job.Cancel);
        else {
            try { job.Run(); } catch (...) { Call(job.Cancel); }
        }
        CurrentCancellation.reset();
        { std::lock_guard lock(Mutex);
          if (job.Type == Kind::Read) ActiveReads.erase(job.Lane);
          else { ActiveMovements.erase(job.Lane); Active.erase(job.Lane); } }
        Wake.notify_all();
    }
}

void PtzRequestExecutor::Stop()
{
    std::deque<Job> Cancelled;
    { std::lock_guard lock(Mutex); Closing = true; Cancelled.swap(Queue);
      for (const auto& [lane, cancellation] : ActiveMovements) cancellation->store(true); }
    for (const auto& job : Cancelled) Call(job.Cancel);
    Wake.notify_all();
    for (auto& worker : Workers) if (worker.joinable()) worker.join();
    Workers.clear();
}
