#include "RecentCaptureJobs.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

bool RecentCaptureJobs::Terminal(State state) {
    return state == State::Completed || state == State::Failed || state == State::Cancelled;
}
RecentCaptureJobs::RecentCaptureJobs(Resolver resolver, Exporter exporter, Clock clock)
    : Resolve(std::move(resolver)), Export(std::move(exporter)), Now(std::move(clock)) {
    if (!Resolve || !Export) throw std::invalid_argument("Capture callbacks are required");
    if (!Now) Now = [] {return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();};
    Thread = std::thread([this] {Worker();});
}
RecentCaptureJobs::~RecentCaptureJobs() { Shutdown(); }
std::optional<uint64_t> RecentCaptureJobs::Submit(Request request) {
    const auto now = Now();
    if (request.Camera <= 0 || request.Owner <= 0 || request.Destination.empty()
        || !std::isfinite(request.From) || !std::isfinite(request.To) || request.From < 0
        || request.From >= now || now - request.From > 21600
        || (request.To != 0 && (request.To <= request.From || request.To > now || request.To - request.From > 21600))
        || request.FinalizationWaitSeconds < 0 || request.FinalizationWaitSeconds > 960) return {};
    std::lock_guard lock(Mutex);
    if (Closing) return {};
    size_t active = 0;
    for (const auto& [id, job] : Jobs) {
        if (!Terminal(job.Value.Status)) {
            ++active;
            if (job.Value.Capture.Destination == request.Destination) return {};
        }
    }
    if (active >= 8) return {};
    if (Jobs.size() >= 64) {
        const auto oldest = std::find_if(Jobs.begin(), Jobs.end(), [](const auto& entry) {return Terminal(entry.second.Value.Status);});
        if (oldest == Jobs.end()) return {};
        Jobs.erase(oldest); // Do not delete completed media when retiring status.
    }
    Job job;
    job.Value.Id = NextId++;
    job.Value.Capture = std::move(request);
    job.Value.Status = job.Value.Capture.To == 0 ? State::Recording : State::WaitingForMedia;
    job.Deadline = now + job.Value.Capture.FinalizationWaitSeconds;
    const auto id = job.Value.Id;
    Jobs.emplace(id, std::move(job)); Wake.notify_all(); return id;
}
std::optional<RecentCaptureJobs::Snapshot> RecentCaptureJobs::Get(uint64_t id, int owner, int camera) const {
    std::lock_guard lock(Mutex);
    const auto found = Jobs.find(id);
    if (found == Jobs.end() || found->second.Value.Capture.Owner != owner || found->second.Value.Capture.Camera != camera) return {};
    return found->second.Value;
}
bool RecentCaptureJobs::StopCapture(uint64_t id, int owner, int camera, double requestTime) {
    if (!std::isfinite(requestTime)) return false;
    const auto now = Now();
    std::lock_guard lock(Mutex);
    const auto found = Jobs.find(id);
    if (found == Jobs.end()) return false;
    auto& job = found->second;
    if (job.Value.Capture.Owner != owner || job.Value.Capture.Camera != camera || job.Value.Status != State::Recording
        || requestTime <= job.Value.Capture.From || requestTime > now) return false;
    job.Value.Capture.To = (std::min)(requestTime, job.Value.Capture.From + 21600);
    job.Value.Status = State::WaitingForMedia;
    job.Deadline = now + job.Value.Capture.FinalizationWaitSeconds;
    job.NextCheck = 0; Wake.notify_all(); return true;
}
bool RecentCaptureJobs::Cancel(uint64_t id, int owner, int camera) {
    std::lock_guard lock(Mutex);
    const auto found = Jobs.find(id);
    if (found == Jobs.end()) return false;
    auto& job = found->second;
    if (job.Value.Capture.Owner != owner || job.Value.Capture.Camera != camera || Terminal(job.Value.Status)) return false;
    job.Cancelled->store(true);
    // An in-flight exporter owns its publication boundary; its result decides
    // whether cancellation beat successful file publication.
    if (job.Value.Status != State::Exporting) job.Value.Status = State::Cancelled;
    Wake.notify_all(); return true;
}
void RecentCaptureJobs::Shutdown() {
    {
        std::lock_guard lock(Mutex);
        Closing = true;
        for (auto& [id, job] : Jobs) {
            job.Cancelled->store(true);
            if (!Terminal(job.Value.Status) && job.Value.Status != State::Exporting) job.Value.Status = State::Cancelled;
        }
    }
    Wake.notify_all(); if (Thread.joinable()) Thread.join();
}
void RecentCaptureJobs::Worker() {
    for (;;) {
        uint64_t id = 0;
        Request request;
        std::shared_ptr<std::atomic<bool>> cancelled;
        double deadline = 0;
        {
            std::unique_lock lock(Mutex);
            if (Closing) return;
            const auto now = Now();
            for (auto& [key, job] : Jobs) {
                if (job.Value.Status == State::Recording && now >= job.Value.Capture.From + 21600) {
                    job.Value.Capture.To = job.Value.Capture.From + 21600;
                    job.Value.Status = State::WaitingForMedia;
                    job.Deadline = now + job.Value.Capture.FinalizationWaitSeconds;
                }
                if (job.Value.Status == State::WaitingForMedia && job.NextCheck <= now) {
                    id = key; request = job.Value.Capture; cancelled = job.Cancelled;
                    deadline = job.Deadline; job.NextCheck = now + 1; break;
                }
            }
            if (!id) {Wake.wait_for(lock, std::chrono::milliseconds(100)); continue;}
        }
        Witness::Camera::HistoricalClipResult result;
        bool ran = false;
        try {
            auto sources = Resolve(request);
            if (sources.Pending && Now() < deadline && !cancelled->load()) continue;
            {
                std::lock_guard lock(Mutex);
                if (cancelled->load() || Closing) continue;
                Jobs.at(id).Value.Status = State::Exporting;
            }
            ran = true;
            result = Export(sources, request, [cancelled] {return cancelled->load();});
        } catch (const std::exception& error) {result.Error = error.what(); ran = true;}
        catch (...) {result.Error = "Unexpected capture failure"; ran = true;}
        if (ran) {
            std::lock_guard lock(Mutex);
            auto& value = Jobs.at(id).Value;
            value.Status = result.Success ? State::Completed : cancelled->load() ? State::Cancelled : State::Failed;
            value.Result = std::move(result);
        }
    }
}
