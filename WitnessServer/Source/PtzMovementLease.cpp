#include "PtzMovementLease.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct PtzMovementLease::Impl
{
    using Clock = std::chrono::steady_clock;
    struct Lane
    {
        std::mutex Mutex;
        std::condition_variable Changed;
        Send PendingStop;
        Clock::time_point Deadline;
        bool Closing = false;
        std::thread Worker;
    };
    std::mutex Mutex;
    std::unordered_map<int, std::shared_ptr<Lane>> Lanes;
    Notice Notify;
    std::chrono::milliseconds Duration, Retry;
    bool Closing = false;
    Impl(Notice notice, std::chrono::milliseconds duration, std::chrono::milliseconds retry)
        : Notify(std::move(notice)), Duration(duration), Retry(retry) {}

    static bool Call(const Send& send) noexcept
    {
        try { return send && send(); } catch (...) { return false; }
    }
    void Report(int device, bool ok) noexcept
    {
        try { if (Notify) Notify(device, ok); } catch (...) {}
    }
    std::shared_ptr<Lane> Get(int device)
    {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Closing || device <= 0) return {};
        auto found = Lanes.find(device);
        if (found != Lanes.end()) return found->second;
        // Bounded even if callers repeatedly rotate IDs. Do not evict a moving lane.
        if (Lanes.size() >= 64) return {};
        auto lane = std::make_shared<Lane>();
        Lanes.emplace(device, lane);
        try { lane->Worker = std::thread([this, lane, device] {
            std::unique_lock<std::mutex> guard(lane->Mutex);
            unsigned failures = 0;
            for (;;)
            {
                if (lane->Closing) break;
                if (!lane->PendingStop) { lane->Changed.wait(guard); continue; }
                if (Clock::now() < lane->Deadline) {
                    lane->Changed.wait_until(guard, lane->Deadline); continue;
                }
                // Serialize the safety stop with incoming commands on THIS device.
                // Other cameras have their own lane; a blocked camera cannot delay them.
                const bool ok = Call(lane->PendingStop);
                if (ok) { lane->PendingStop = {}; failures = 0; Report(device, true); }
                else {
                    if (failures++ % 10 == 0) Report(device, false);
                    lane->Deadline = Clock::now() + Retry;
                }
            }
            if (lane->PendingStop) Report(device, Call(lane->PendingStop));
        }); } catch (...) { Lanes.erase(device); throw; }
        return lane;
    }
    ~Impl()
    {
        std::vector<std::shared_ptr<Lane>> lanes;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            Closing = true;
            for (auto& item : Lanes) lanes.push_back(item.second);
        }
        for (auto& lane : lanes) {
            std::lock_guard<std::mutex> guard(lane->Mutex);
            lane->Closing = true;
            lane->Changed.notify_all();
        }
        for (auto& lane : lanes) if (lane->Worker.joinable()) lane->Worker.join();
    }
};

PtzMovementLease::PtzMovementLease(Notice notice, std::chrono::milliseconds duration,
    std::chrono::milliseconds retry) : m_Impl(std::make_unique<Impl>(std::move(notice),
        duration.count() > 0 ? duration : std::chrono::seconds(10),
        retry.count() > 0 ? retry : std::chrono::seconds(1))) {}
PtzMovementLease::~PtzMovementLease() = default;

bool PtzMovementLease::Move(int device, Send command, Send stop)
{
    auto lane = m_Impl->Get(device);
    if (!lane || !command || !stop) return false;
    std::lock_guard<std::mutex> lock(lane->Mutex);
    if (lane->Closing) return false;
    // Do not supersede an ambiguous prior movement without a successful Stop.
    if (lane->PendingStop && !Impl::Call(lane->PendingStop)) return false;
    lane->PendingStop = std::move(stop);
    lane->Deadline = Impl::Clock::now() + m_Impl->Duration;
    lane->Changed.notify_all();
    const bool ok = Impl::Call(command);
    if (!ok) lane->Deadline = Impl::Clock::now();
    return ok;
}
bool PtzMovementLease::Stop(int device, Send stop)
{
    auto lane = m_Impl->Get(device);
    if (!lane || !stop) return false;
    std::lock_guard<std::mutex> lock(lane->Mutex);
    if (lane->Closing) return false;
    // Keep the original transport for a movement if the new request resolved a
    // different client. It is the one which may actually have started the motor.
    const bool ok = Impl::Call(lane->PendingStop ? lane->PendingStop : stop);
    if (ok) lane->PendingStop = {};
    else { if (!lane->PendingStop) lane->PendingStop = std::move(stop); lane->Deadline = Impl::Clock::now() + m_Impl->Retry; }
    lane->Changed.notify_all();
    return ok;
}
bool PtzMovementLease::Position(int device, Send command)
{
    auto lane = m_Impl->Get(device);
    if (!lane || !command) return false;
    std::lock_guard<std::mutex> lock(lane->Mutex);
    if (lane->Closing) return false;
    if (lane->PendingStop) {
        if (!Impl::Call(lane->PendingStop)) return false;
        lane->PendingStop = {};
        lane->Changed.notify_all();
    }
    return Impl::Call(command);
}
