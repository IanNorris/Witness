#include "RecentCaptureJobs.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
template<class F> void Await(F predicate) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < limit, "Capture test timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
int main() {
    try {
        std::atomic<double> now{1000};
        std::atomic<bool> pending{true};
        std::atomic<int> exports{0};
        std::atomic<bool> entered{false}, release{false};
        RecentCaptureJobs jobs([&](const auto& request) {
            RecentCaptureJobs::Sources sources;
            sources.Pending = request.Camera == 1 && pending.load(); return sources;
        }, [&](const auto&, const auto& request, const auto& cancelled) {
            ++exports;
            if (request.Camera == 3) {
                entered = true;
                while (!release.load() && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Witness::Camera::HistoricalClipResult result;
            result.Success = !cancelled(); return result;
        }, [&] {return now.load();});
        auto request = [](int camera, bool continuing = false) {
            RecentCaptureJobs::Request value;
            value.Camera = camera; value.Owner = 7; value.From = 900;
            value.To = continuing ? 0 : 999; value.Destination = "capture-" + std::to_string(camera);
            return value;
        };
        const auto waiting = jobs.Submit(request(1)); Check(waiting.has_value(), "Admission failed");
        const auto ready = jobs.Submit(request(2)); Check(ready.has_value(), "Independent admission failed");
        Await([&] {return jobs.Get(*ready, 7, 2)->Status == RecentCaptureJobs::State::Completed;});
        Check(exports == 1 && jobs.Get(*waiting, 7, 1)->Status == RecentCaptureJobs::State::WaitingForMedia,
            "Waiting media blocked a ready capture or exported too early");
        Check(!jobs.Get(*ready, 8, 2) && !jobs.Get(*ready, 7, 1), "Job ownership/camera check failed");
        pending = false; now = 1002;
        Await([&] {return jobs.Get(*waiting, 7, 1)->Status == RecentCaptureJobs::State::Completed;});
        const auto continuing = jobs.Submit(request(4, true)); Check(continuing.has_value(), "Continue admission failed");
        Check(jobs.Get(*continuing, 7, 4)->Status == RecentCaptureJobs::State::Recording
            && !jobs.StopCapture(*continuing, 8, 4, 1001) && !jobs.StopCapture(*continuing, 7, 4, 1003), "Unauthorized/future stop accepted");
        Check(jobs.StopCapture(*continuing, 7, 4, 1001), "Continue stop failed");
        Await([&] {return jobs.Get(*continuing, 7, 4)->Status == RecentCaptureJobs::State::Completed;});
        Check(jobs.Get(*continuing, 7, 4)->Capture.To == 1001, "Stop boundary changed after request");
        const auto cancellable = jobs.Submit(request(3)); Check(cancellable.has_value(), "Cancellation admission failed");
        Await([&] {return entered.load();});
        Check(jobs.Cancel(*cancellable, 7, 3), "Export cancellation failed"); release = true;
        Await([&] {return jobs.Get(*cancellable, 7, 3)->Status == RecentCaptureJobs::State::Cancelled;});
        std::vector<uint64_t> held;
        for (int camera = 10; camera < 18; ++camera) {
            auto id = jobs.Submit(request(camera, true)); Check(id.has_value(), "Bounded admission rejected early"); held.push_back(*id);
        }
        Check(!jobs.Submit(request(18, true)), "Active capacity limit exceeded");
        jobs.Shutdown();
        for (size_t index = 0; index < held.size(); ++index)
            Check(jobs.Get(held[index], 7, static_cast<int>(index) + 10)->Status == RecentCaptureJobs::State::Cancelled,
                "Shutdown left a pending capture live");
        Check(!jobs.Submit(request(19)), "Shutdown accepted new work");
        std::atomic<int> forbiddenExports{0};
        RecentCaptureJobs::Persistence broken;
        broken.Save = [](const auto&) {throw std::runtime_error("Synthetic journal failure");};
        RecentCaptureJobs unavailable([](const auto&) {return RecentCaptureJobs::Sources{};},
            [&](const auto&, const auto&, const auto&) {++forbiddenExports; return Witness::Camera::HistoricalClipResult{};},
            [&] {return now.load();}, std::move(broken));
        const auto failed = unavailable.Submit(request(20)); Check(failed.has_value(), "Journal test admission failed");
        Await([&] {return unavailable.Get(*failed, 7, 20)->Status == RecentCaptureJobs::State::Failed;});
        Check(forbiddenExports == 0 && !unavailable.Submit(request(21)), "Journal failure allowed an untracked export");
        unavailable.Shutdown();
        std::atomic<bool> liveFinished{false};
        std::shared_ptr<std::atomic<double>> liveStop;
        std::shared_ptr<std::atomic<bool>> liveCancel;
        RecentCaptureJobs monitored([&](const auto&) {
            RecentCaptureJobs::Sources sources; sources.LiveStopped = liveFinished.load(); return sources;
        }, [](const auto&, const auto&, const auto&) {
            Witness::Camera::HistoricalClipResult result; result.Success = true; return result;
        }, [&] {return now.load();}, {}, [&](const auto&, auto cancelled, auto stopAt) {
            liveCancel = std::move(cancelled); liveStop = std::move(stopAt);
        });
        const auto stopped = monitored.Submit(request(22, true)); Check(stopped.has_value(), "Live monitoring admission failed");
        Check(liveStop && liveStop->load() == 0 && liveCancel && !liveCancel->load(), "Independent producer controls were not dispatched");
        liveFinished = true; now = 1004;
        Await([&] {return monitored.Get(*stopped, 7, 22)->Status == RecentCaptureJobs::State::Completed;});
        Check(monitored.Get(*stopped, 7, 22)->Capture.To == 1004 && liveStop->load() == 1004,
            "Unexpected producer stop left the job claiming to record");
        monitored.Shutdown();
        std::cout << "PASS background captures: independent wait, frozen stop, owner checks, bounds, cancellation, shutdown\n";
        return 0;
    } catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
}
