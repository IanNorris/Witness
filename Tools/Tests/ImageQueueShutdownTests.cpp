// Public Camera DLL API only; no camera, database, model or network access.
#include "ImageProcessingJob.h"
#include "LiveOutputStream.h"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

using namespace Witness::Camera;
using namespace std::chrono_literals;

static bool FreshWorkerWaitsUntilStopped()
{
    ImageProcessingJobQueue Queue;
    std::promise<void> Entering, Returned;
    auto Started = Entering.get_future();
    auto Done = Returned.get_future();
    std::thread Worker([&] {
        Entering.set_value();
        Queue.WorkerThreadMain();
        Returned.set_value();
    });
    Started.wait();
    const bool Waited = Done.wait_for(100ms) == std::future_status::timeout;
    Queue.RequestShutdown();
    const bool Stopped = Done.wait_for(500ms) == std::future_status::ready;
    if (!Stopped) Queue.Push(nullptr, true);
    Worker.join();
    return Waited && Stopped;
}

static bool LateWorkerKeepsStopRequest()
{
    ImageProcessingJobQueue Queue;
    Queue.RequestShutdown();
    Queue.RequestShutdown(); // idempotent stop before any worker enters
    std::promise<void> Returned;
    auto Done = Returned.get_future();
    std::thread Worker([&] {
        Queue.WorkerThreadMain();
        Returned.set_value();
    });
    const bool Stopped = Done.wait_for(500ms) == std::future_status::ready;
    if (!Stopped) {
        // Legacy code clears the stop flag on entry. Unblock it through the
        // existing public sentinel so a failing regression cannot hang CI.
        Queue.Push(nullptr, true);
    }
    Worker.join();
    return Stopped;
}

static bool StopReleasesAllWaiters()
{
    ImageProcessingJobQueue Queue;
    std::vector<std::thread> Workers;
    std::vector<std::future<void>> Done;
    for (int Index = 0; Index < 8; ++Index) {
        std::promise<void> Returned;
        Done.push_back(Returned.get_future());
        Workers.emplace_back([&, Returned = std::move(Returned)]() mutable {
            Queue.WorkerThreadMain();
            Returned.set_value();
        });
    }
    // A stop request must work both for waiting workers and workers which have
    // not yet entered the queue; no timing assumption about thread scheduling.
    Queue.RequestShutdown();
    bool Stopped = true;
    for (auto& Result : Done)
        Stopped = (Result.wait_for(500ms) == std::future_status::ready) && Stopped;
    if (!Stopped) {
        Queue.Push(nullptr, true);
        Queue.RequestShutdown(); // wake every waiter, not just sentinel's notify_one
    }
    for (auto& Worker : Workers) Worker.join();
    return Stopped;
}

int main()
{
    std::cout << "Camera build=" << LiveOutputStream::GetNativeBuildIdentity() << std::endl;
    const bool Fresh = FreshWorkerWaitsUntilStopped();
    const bool Late = LateWorkerKeepsStopRequest();
    const bool Waiters = StopReleasesAllWaiters();
    if (!Fresh || !Late || !Waiters) {
        std::cerr << "FAIL queue stop: fresh wait=" << Fresh
                  << " late entry=" << Late << " all waiters=" << Waiters << '\n';
        return 1;
    }
    std::cout << "PASS: queue stop survives late worker entry and releases all waiters\n";
    return 0;
}
