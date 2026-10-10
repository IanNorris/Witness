#include "RecentCaptureJobs.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

bool RecentCaptureJobs::Terminal(State state) {
    return state == State::Completed || state == State::Failed || state == State::Cancelled;
}
RecentCaptureJobs::RecentCaptureJobs(Resolver resolver, Exporter exporter, Clock clock, Persistence persistence)
    : Resolve(std::move(resolver)), Export(std::move(exporter)), Now(std::move(clock)), Journal(std::move(persistence)) {
    if (!Resolve || !Export) throw std::invalid_argument("Capture callbacks are required");
    if (!Now) Now = [] {return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();};
    NextId = static_cast<uint64_t>(Now() * 1000000);
    if (Journal.Recovered.size() > 64) throw std::invalid_argument("Capture journal exceeds capacity");
    for (auto& value : Journal.Recovered) {
        if (!value.Id || value.Id >= UINT64_MAX - 1 || Jobs.count(value.Id)) throw std::invalid_argument("Invalid recovered capture ID");
        NextId = (std::max)(NextId, value.Id + 1);
        if (!Terminal(value.Status)) {
            value.Status = State::Failed;
            value.Result.Success = false;
            value.Result.Error = "Interrupted by server restart. Existing media has not been overwritten; start a new capture if needed.";
        }
        Job job; job.Value = std::move(value); Jobs.emplace(job.Value.Id, std::move(job));
    }
    if (!Jobs.empty()) ++Revision;
    Journal.Recovered.clear();
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
    if (Closing || JournalFailed) return {};
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
    Jobs.emplace(id, std::move(job)); ++Revision; Wake.notify_all(); return id;
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
    job.NextCheck = 0; ++Revision; Wake.notify_all(); return true;
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
    ++Revision;
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
        ++Revision;
    }
    Wake.notify_all(); if (Thread.joinable()) Thread.join();
    if (Journal.Save) {
        std::vector<Snapshot> values;
        {std::lock_guard lock(Mutex); for (const auto& [id, job] : Jobs) values.push_back(job.Value);}
        // Destruction must not throw. Worker persistence failures are surfaced
        // in job status; on restart, an uncheckpointed active job is interrupted.
        try {Journal.Save(values);} catch (...) {}
    }
}
void RecentCaptureJobs::Worker() {
    for (;;) {
        std::vector<Snapshot> checkpoint;
        uint64_t revision = 0;
        {
            std::lock_guard lock(Mutex);
            if (Closing) return;
            if (Journal.Save && Revision != SavedRevision) {
                revision = Revision;
                for (const auto& [id, job] : Jobs) checkpoint.push_back(job.Value);
            }
        }
        if (revision) {
            try {
                Journal.Save(checkpoint);
                std::lock_guard lock(Mutex); SavedRevision = revision; JournalFailed = false;
            } catch (...) {
                std::unique_lock lock(Mutex); JournalFailed = true;
                for (auto& [id, job] : Jobs) if (!Terminal(job.Value.Status)) {
                    job.Cancelled->store(true); job.Value.Status = State::Failed;
                    job.Value.Result.Error = "Capture journal could not be saved; no export was started.";
                }
                ++Revision;
                Wake.wait_for(lock, std::chrono::seconds(1), [this] {return Closing;});
                continue;
            }
        }
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
                    ++Revision;
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
                ++Revision;
            }
            ran = true;
            result = Export(sources, request, [cancelled] {return cancelled->load();});
        } catch (const std::exception& error) {result.Error = error.what(); ran = true;}
        catch (...) {result.Error = "Unexpected capture failure"; ran = true;}
        if (ran) {
            std::lock_guard lock(Mutex);
            const auto found = Jobs.find(id);
            // A cancelled job waiting in the resolver can be retired while the
            // resolver unwinds. Never let its late exception kill the worker.
            if (found == Jobs.end()) continue;
            auto& value = found->second.Value;
            value.Status = result.Success ? State::Completed : cancelled->load() ? State::Cancelled : State::Failed;
            value.Result = std::move(result);
            ++Revision;
        }
    }
}
