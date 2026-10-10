#include "CrowListener.h"
#include "CrowAuth.h"
#include "AuthHelpers.h"
#include "RecentCaptureJournal.h"
#include "CapturedClipStorage.h"
#include "ClipHelpers.h"
#include "EventBroadcaster.h"
#include "CameraWorker.h"
#include "RecentCaptureLive.h"
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>
#include <ContinuousOutputStream.h>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <cmath>

namespace {
double RequestTime() {return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();}
const char* StateName(RecentCaptureJobs::State state) {
    switch (state) {
    case RecentCaptureJobs::State::Recording: return "recording";
    case RecentCaptureJobs::State::WaitingForMedia: return "currentlySaving";
    case RecentCaptureJobs::State::Exporting: return "exporting";
    case RecentCaptureJobs::State::Completed: return "completed";
    case RecentCaptureJobs::State::Cancelled: return "cancelled";
    default: return "failed";
    }
}
void JobResponse(crow::response& res, const RecentCaptureJobs::Snapshot& job) {
    crow::json::wvalue value;
    value["id"] = std::to_string(job.Id); value["state"] = StateName(job.Status);
    value["from"] = job.Capture.From; value["to"] = job.Capture.To;
    value["actualFrom"] = job.Result.ActualFrom; value["actualTo"] = job.Result.ActualTo;
    value["error"] = job.Result.Error;
    std::vector<crow::json::wvalue> coverage, gaps;
    for (const auto& range : job.Result.Coverage) {crow::json::wvalue item; item["from"] = range.From; item["to"] = range.To; coverage.push_back(std::move(item));}
    for (const auto& range : job.Result.Gaps) {crow::json::wvalue item; item["from"] = range.From; item["to"] = range.To; gaps.push_back(std::move(item));}
    value["coverage"] = std::move(coverage); value["gaps"] = std::move(gaps);
    // Passthrough boundaries are truthful, not advertised as frame-exact cuts.
    value["boundaryMode"] = "keyframePassthrough";
    res.set_header("Content-Type", "application/json");
    res.set_header("Cache-Control", "no-store"); res.body = value.dump(); res.end();
}
}
void CrowListener::StartRecentCaptures() {
    if (m_RecentCaptures) m_RecentCaptures->Shutdown();
    const auto context = m_GlobalContext;
    const auto journal = std::make_shared<RecentCaptureJournal>((std::filesystem::path(context->CachePath) / "captures" / "capture-jobs.sqlite").string());
    RecentCaptureJobs::Persistence persistence;
    persistence.Recovered = journal->Load();
    persistence.Save = [journal](const auto& values) {journal->Save(values);};
    const auto liveRequests = std::make_shared<RecentCaptureLiveRegistry>();
    m_RecentCaptures = std::make_unique<RecentCaptureJobs>([context, liveRequests](const RecentCaptureJobs::Request& request) {
        RecentCaptureJobs::Sources result;
        const auto live = liveRequests->Find(request.Destination);
        if (!live) {result.Pending = true; return result;} // Admission/start dispatch race.
        const auto liveState = live->Get();
        if (!liveState.Ready) {result.Pending = true; return result;}
        result.Error = liveState.Error;
        result.LiveStopped = liveState.Finished && !liveState.UseDvr;
        if (request.To == 0) return result; // Lightweight monitoring, no DVR query.
        {
            SQLiteDatabaseQueryInstance query(context->Database, "SelectCaptureContinuousSegments");
            query->Bind("@CameraUID", request.Camera);
            query->Bind("@TimestampFrom", static_cast<int64_t>(std::floor(request.From)));
            query->Bind("@TimestampTo", static_cast<int64_t>(std::ceil(request.To)));
            query->Execute([&](const SQLiteDatabaseQuery& row) {
                const char* path = row.GetColumnValueText(0);
                if (path) result.Files.push_back({path, row.GetColumnValueDouble(1), row.GetColumnValueDouble(2), 0, row.GetColumnValueDouble(3)});
                return result.Files.size() < 1025;
            });
        } // Release the database query before touching any media files.
        const auto active = context->GetActiveDvrSegment(request.Camera);
        const double start = active.MediaStartTimestamp > 0 ? active.MediaStartTimestamp : static_cast<double>(active.StartTimestamp);
        const double end = active.MediaEndTimestamp > 0 ? active.MediaEndTimestamp : active.StartTimestamp + active.ObservedDuration;
        result.Pending = !active.FilePath.empty() && start < request.To && end > request.From;
        if (!liveState.UseDvr) {
            result.Files.insert(result.Files.end(), liveState.Files.begin(), liveState.Files.end());
            result.Pending = !liveState.Finished;
        }
        // Normal, nonfragmented files must finalize before export. Completed
        // fragments will be admitted here only after their production rollout.
        return result;
    }, [context, liveRequests](const RecentCaptureJobs::Sources& sources, const RecentCaptureJobs::Request& request, const auto& cancelled) {
        std::filesystem::create_directories(std::filesystem::path(request.Destination).parent_path());
        auto result = Witness::Camera::RemuxHistoricalClip(sources.Files, request.From, request.To, request.Destination, cancelled);
        if (result.Success && !sources.Error.empty()) result.Error = sources.Error;
        if (result.Success) {
            const char* databasePath = sqlite3_db_filename(context->Database->GetDatabase(), "main");
            if (!databasePath) throw std::runtime_error("Capture clip database path is unavailable");
            const auto uid = RegisterCapturedClip(databasePath, request.Camera, request.Destination, result, request.From, request.To);
            try {
                cv::VideoCapture video(request.Destination, cv::CAP_FFMPEG);
                cv::Mat frame;
                if (video.read(frame) && !frame.empty()) {
                    auto thumbnail = std::filesystem::path(request.Destination); thumbnail.replace_extension(".jpg");
                    cv::imwrite(thumbnail.string(), frame);
                }
            } catch (...) { /* A missing thumbnail does not discard saved media. */ }
            if (context->Events) {
                crow::json::wvalue event; event["clipUID"] = uid; event["cameraID"] = request.Camera;
                event["timestamp"] = static_cast<int64_t>(result.ActualFrom); event["duration"] = result.ActualTo - result.TimelineFrom;
                event["tags"] = ""; context->Events->Broadcast("clip:new", std::move(event));
            }
        }
        if (const auto live = liveRequests->Find(request.Destination)) {
            const auto state = live->Get();
            if (state.Finished && !state.UseDvr) {
                for (const auto& file : state.Files) {std::error_code error; std::filesystem::remove(file.FilePath, error);}
                std::error_code error; std::filesystem::remove(live->Directory, error);
            }
        }
        liveRequests->Remove(request.Destination);
        return result;
    }, RecentCaptureJobs::Clock{}, std::move(persistence), [context, liveRequests](const auto& request, auto cancelled, auto stopAt) {
        auto live = std::make_shared<RecentCaptureLive>();
        live->Directory = request.Destination + ".live";
        live->Cancelled = std::move(cancelled); live->StopAt = std::move(stopAt);
        liveRequests->Add(request.Destination, live);
        std::shared_ptr<CameraWorker> worker;
        {
            std::shared_lock lock(context->Mutex);
            const auto camera = context->GetCameraMap().find(request.Camera);
            if (camera != context->GetCameraMap().end()) worker = camera->second.Worker;
        }
        if (!worker) throw std::runtime_error("Camera worker is unavailable");
        context->MessageBus->SendToClient(worker.get(), std::make_shared<RecentCaptureLiveMessage>(live));
    });
}
void CrowListener::HandleCapturedClipMedia(const crow::request& req, crow::response& res, int clipUID, const std::string& type) {
    if (type != "video" && type != "thumbnail" && type != "metadata") {res.code = 404; res.end(); return;}
    int camera = 0;
    {
        SQLiteDatabaseQueryInstance query(m_GlobalContext->Database, "SelectClipID");
        query->Bind("@ClipUID", clipUID);
        query->Execute([&](const SQLiteDatabaseQuery& row) {camera = row.GetColumnValueInt(2); return false;});
    }
    if (camera <= 0) {res.code = 404; res.end(); return;}
    if (CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, nullptr, CrowAuth::Action::Read, CrowAuth::Privilege::Normal, camera) <= 0) {
        res.code = 403; res.end(); return;
    }
    if (type == "metadata") {
        crow::json::wvalue value;
        bool found = false;
        SQLiteDatabaseQueryInstance query(m_GlobalContext->Database, "SelectCapturedClipMedia");
        query->Bind("@ClipUID", clipUID);
        query->Execute([&](const SQLiteDatabaseQuery& row) {
            found = true;
            value["requestedFrom"] = row.GetColumnValueDouble(2); value["requestedTo"] = row.GetColumnValueDouble(3);
            value["actualFrom"] = row.GetColumnValueDouble(4); value["actualTo"] = row.GetColumnValueDouble(5);
            value["timelineFrom"] = row.GetColumnValueDouble(6);
            for (const auto& [name, index] : {std::pair{"coverage", 7}, std::pair{"gaps", 8}}) {
                const auto text = row.GetColumnValueText(index);
                const auto parsed = crow::json::load(text ? text : "[]");
                if (!parsed) {res.code = 500; return false;}
                value[name] = crow::json::wvalue(parsed);
            }
            return false;
        });
        if (!found) res.code = 404;
        if (found && res.code != 500) {res.set_header("Content-Type", "application/json"); res.body = value.dump();}
        res.set_header("Cache-Control", "private, no-store"); res.end(); return;
    }
    const auto path = GetCapturedClipPath(m_GlobalContext->Database, clipUID, type == "video");
    if (path.empty()) {res.code = 404; res.end(); return;}
    res.set_static_file_info_unsafe(path, type == "video" ? "video/mp4" : "image/jpeg");
    res.set_header("Cache-Control", "private, no-store"); res.end();
}
void CrowListener::HandleRecentCaptureCreate(const crow::request& req, crow::response& res, int cameraId) {
    const double now = RequestTime(); // Freeze before auth/database waits.
    if (req.body.size() > 4096) {res.code = 413; res.end(); return;}
    auto body = crow::json::load(req.body);
    if (!body) {res.code = 400; res.end(); return;}
    const int owner = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, &body,
        CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Normal, cameraId);
    if (owner <= 0) {res.code = 403; res.end(); return;}
    if (!m_RecentCaptures) {res.code = 503; res.end(); return;}
    RecentCaptureJobs::Request capture;
    capture.Camera = cameraId; capture.Owner = owner;
    try {
        if (body.has("from")) capture.From = body["from"].d();
        else if (body.has("lookbackSeconds")) capture.From = now - body["lookbackSeconds"].d();
        else {res.code = 400; res.end(); return;}
        const bool continuing = body.has("continueRecording") && body["continueRecording"].b();
        capture.To = continuing ? 0 : body.has("to") ? body["to"].d() : now;
    } catch (...) {res.code = 400; res.end(); return;}
    capture.FinalizationWaitSeconds = m_GlobalContext->DvrSegmentSeconds + 30;
    capture.Destination = (std::filesystem::path(m_GlobalContext->CachePath) / "captures" /
        (std::to_string(cameraId) + "_" + GetRandomToken() + ".mp4")).string();
    const auto id = m_RecentCaptures->Submit(std::move(capture));
    if (!id) {res.code = 422; res.body = "Invalid window or capture queue full"; res.end(); return;}
    res.code = 202; JobResponse(res, *m_RecentCaptures->Get(*id, owner, cameraId));
}
void CrowListener::HandleRecentCaptureJob(const crow::request& req, crow::response& res, int cameraId,
    const std::string& idText, const std::string& action) {
    const double now = RequestTime();
    const bool write = action == "stop" || action == "cancel";
    if ((write && req.method != crow::HTTPMethod::POST) || (!write && req.method != crow::HTTPMethod::GET)
        || (!write && action != "status" && action != "download")) {res.code = 405; res.end(); return;}
    if (req.body.size() > 4096) {res.code = 413; res.end(); return;}
    auto body = crow::json::load(req.body);
    if (write && !body) {res.code = 400; res.end(); return;}
    const int owner = CrowAuth::IsCameraAuthenticated(*m_GlobalContext, req, write ? &body : nullptr,
        write ? CrowAuth::Action::ReadWrite : CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId);
    if (owner <= 0) {res.code = 403; res.end(); return;}
    uint64_t id = 0;
    const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
    if (idText.empty() || parsed.ec != std::errc{} || parsed.ptr != idText.data() + idText.size()) {res.code = 400; res.end(); return;}
    const auto job = m_RecentCaptures ? m_RecentCaptures->Get(id, owner, cameraId) : std::nullopt;
    if (!job) {res.code = 404; res.end(); return;}
    if (write) {
        const bool success = action == "stop" ? m_RecentCaptures->StopCapture(id, owner, cameraId, now)
            : m_RecentCaptures->Cancel(id, owner, cameraId);
        if (!success) {res.code = 409; res.end(); return;}
        JobResponse(res, *m_RecentCaptures->Get(id, owner, cameraId)); return;
    }
    if (action == "download") {
        if (job->Status != RecentCaptureJobs::State::Completed) {res.code = 409; res.end(); return;}
        res.set_static_file_info_unsafe(job->Capture.Destination, "video/mp4");
        res.set_header("Cache-Control", "no-store");
        res.set_header("Content-Disposition", "attachment; filename=\"camera-" + std::to_string(cameraId) + "-capture.mp4\"");
        res.end(); return;
    }
    JobResponse(res, *job);
}
