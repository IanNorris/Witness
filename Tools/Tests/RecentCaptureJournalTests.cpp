#include "RecentCaptureJournal.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
int main(int argc, char** argv) {
    try {
        Check(argc == 2 && !std::filesystem::exists(argv[1]), "Usage: RecentCaptureJournalTests new-directory");
        const auto directory = std::filesystem::absolute(argv[1]);
        const auto filename = (directory / "capture-jobs.sqlite").string();
        RecentCaptureJobs::Snapshot recording;
        recording.Id = 123; recording.Capture.Camera = 1; recording.Capture.Owner = 7;
        recording.Capture.From = 900; recording.Capture.To = 0;
        recording.Capture.Destination = (directory / "recording.mp4").string();
        recording.Status = RecentCaptureJobs::State::Recording;
        auto completed = recording;
        completed.Id = 124; completed.Capture.To = 950; completed.Status = RecentCaptureJobs::State::Completed;
        completed.Result.Success = true; completed.Result.ActualFrom = 899; completed.Result.ActualTo = 950.1;
        completed.Result.VideoPackets = 751; completed.Result.AudioPackets = 2348;
        completed.Result.Coverage = {{899, 920}, {930, 950.1}}; completed.Result.Gaps = {{920, 930}};
        {
            RecentCaptureJournal journal(filename);
            journal.Save({recording, completed});
            bool failed = false;
            try {journal.Save({recording, recording});} catch (...) {failed = true;}
            Check(failed && journal.Load().size() == 2, "Failed checkpoint destroyed prior data");
        }
        const auto journal = std::make_shared<RecentCaptureJournal>(filename);
        auto restored = journal->Load();
        Check(restored.size() == 2 && restored[1].Result.Success && restored[1].Result.Gaps.size() == 1
            && restored[1].Result.AudioPackets == 2348 && restored[1].Result.ActualTo == 950.1,
            "Completed capture metadata did not survive reopening");
        RecentCaptureJobs::Persistence persistence;
        persistence.Recovered = restored;
        persistence.Save = [journal](const auto& values) {journal->Save(values);};
        RecentCaptureJobs jobs([](const auto&) {return RecentCaptureJobs::Sources{};},
            [](const auto&, const auto&, const auto&) {return Witness::Camera::HistoricalClipResult{};},
            [] {return 1000.0;}, std::move(persistence));
        Check(jobs.Get(123, 7, 1)->Status == RecentCaptureJobs::State::Failed
            && jobs.Get(123, 7, 1)->Result.Error.find("restart") != std::string::npos,
            "Interrupted capture was mistaken for a live or completed job");
        Check(jobs.Get(124, 7, 1)->Status == RecentCaptureJobs::State::Completed
            && !jobs.Get(124, 8, 1), "Recovered ownership/completed status failed");
        auto request = recording.Capture;
        request.Destination = (directory / "new.mp4").string();
        auto id = jobs.Submit(request);
        Check(id && *id > 124, "Restart reused a previous capture ID");
        jobs.Shutdown();
        Check(journal->Load().size() == 3, "Shutdown did not checkpoint job status");
        std::cout << "PASS capture journal: reopen, metadata, transactional rollback, interrupted state, ownership, IDs, shutdown\n";
        return 0;
    } catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
}
