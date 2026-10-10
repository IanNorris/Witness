#pragma once
#include <HistoricalClip.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// Independent of camera recording messages and HTTP response lifetimes.
// Callbacks execute on one bounded background worker, never under the job lock.
class RecentCaptureJobs {
public:
    enum class State { Recording, WaitingForMedia, Exporting, Completed, Failed, Cancelled };
    struct Request {
        int Camera = 0;
        int Owner = 0;
        double From = 0;
        double To = 0; // Zero means continue until StopCapture or the six-hour cap.
        std::string Destination; // Trusted server-generated path only.
        int FinalizationWaitSeconds = 90;
    };
    struct Snapshot {
        uint64_t Id = 0;
        Request Capture;
        State Status = State::WaitingForMedia;
        Witness::Camera::HistoricalClipResult Result;
    };
    struct Sources {
        std::vector<Witness::Camera::HistoricalClipSource> Files;
        bool Pending = false; // The requested end intersects footage still saving.
    };
    using Resolver = std::function<Sources(const Request&)>;
    using Exporter = std::function<Witness::Camera::HistoricalClipResult(const Sources&, const Request&, const std::function<bool()>&)>;
    using Clock = std::function<double()>;
    RecentCaptureJobs(Resolver resolver, Exporter exporter, Clock clock = {});
    ~RecentCaptureJobs();
    RecentCaptureJobs(const RecentCaptureJobs&) = delete;
    RecentCaptureJobs& operator=(const RecentCaptureJobs&) = delete;
    std::optional<uint64_t> Submit(Request request);
    std::optional<Snapshot> Get(uint64_t id, int owner, int camera) const;
    bool StopCapture(uint64_t id, int owner, int camera, double requestTime);
    bool Cancel(uint64_t id, int owner, int camera);
    void Shutdown();
private:
    struct Job {
        Snapshot Value;
        std::shared_ptr<std::atomic<bool>> Cancelled = std::make_shared<std::atomic<bool>>(false);
        double Deadline = 0;
        double NextCheck = 0;
    };
    void Worker();
    static bool Terminal(State state);
    Resolver Resolve;
    Exporter Export;
    Clock Now;
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::map<uint64_t, Job> Jobs;
    std::thread Thread;
    uint64_t NextId = 1;
    bool Closing = false;
};
