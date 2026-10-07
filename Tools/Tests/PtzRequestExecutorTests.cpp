#include "PtzRequestExecutor.h"
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main()
{
    try {
        PtzRequestExecutor pool;
        pool.Start();
        std::promise<void> entered, release, stopped, independent;
        auto held = release.get_future().share();
        std::atomic<int> moves{0}, cancelled{0}, reads{0};
        Check(pool.Submit({10, PtzRequestExecutor::Kind::Movement, [&] { entered.set_value(); held.wait(); }, [] {}}), "control admission");
        Check(entered.get_future().wait_for(2s) == std::future_status::ready, "read not started");
        Check(pool.Submit({10, PtzRequestExecutor::Kind::Movement, [&] { ++moves; }, [&] { ++cancelled; }}), "move admission");
        Check(pool.Submit({10, PtzRequestExecutor::Kind::Write, [&] { ++reads; }, [] {}}), "queued write admission");
        Check(pool.Submit({10, PtzRequestExecutor::Kind::Stop, [&] { Check(reads == 0, "Stop did not precede queued read"); stopped.set_value(); }, [] {}}), "Stop admission");
        Check(pool.Submit({12, PtzRequestExecutor::Kind::Read, [&] { independent.set_value(); }, [] {}}), "other camera admission");
        const bool progressed = independent.get_future().wait_for(2s) == std::future_status::ready;
        release.set_value();
        Check(progressed, "stalled camera blocked unrelated lane");
        Check(stopped.get_future().wait_for(2s) == std::future_status::ready, "Stop not dispatched");
        pool.Stop();
        Check(moves == 0 && cancelled == 1, "queued movement survived Stop");
        Check(!pool.Submit({10, PtzRequestExecutor::Kind::Read, [] {}, [] {}}), "closed executor admitted work");

        // Stop arriving during discovery cancels the motor send, not only
        // gestures still waiting in the queue.
        pool.Start();
        std::promise<void> discovering, resolve, afterStop;
        auto resolved = resolve.get_future().share();
        pool.Submit({10, PtzRequestExecutor::Kind::Movement, [&] {
            discovering.set_value(); resolved.wait();
            if (!PtzRequestExecutor::MovementCancelled()) ++moves;
        }, [] {}});
        Check(discovering.get_future().wait_for(2s) == std::future_status::ready, "discovery not started");
        pool.Submit({10, PtzRequestExecutor::Kind::Stop, [&] { afterStop.set_value(); }, [] {}});
        resolve.set_value();
        Check(afterStop.get_future().wait_for(2s) == std::future_status::ready, "Stop after discovery not dispatched");
        pool.Stop();
        Check(moves == 0, "motor started after Stop during discovery");

        pool.Start();
        std::promise<void> polling, finishPoll, independentStop;
        auto pollReady = finishPoll.get_future().share();
        pool.Submit({10, PtzRequestExecutor::Kind::Read, [&] { polling.set_value(); pollReady.wait(); }, [] {}});
        Check(polling.get_future().wait_for(2s) == std::future_status::ready, "poll not started");
        pool.Submit({10, PtzRequestExecutor::Kind::Stop, [&] { independentStop.set_value(); }, [] {}});
        const bool stopDuringPoll = independentStop.get_future().wait_for(2s) == std::future_status::ready;
        finishPoll.set_value(); pool.Stop();
        Check(stopDuringPoll, "polling occupied the physical camera's control lane");

        pool.Start();
        std::promise<void> expired;
        pool.Submit({10, PtzRequestExecutor::Kind::Movement, [&] { ++moves; }, [&] { expired.set_value(); },
            std::chrono::steady_clock::now() - 3s});
        Check(expired.get_future().wait_for(2s) == std::future_status::ready, "stale gesture not rejected");
        pool.Stop(); Check(moves == 0, "expired gesture reached motor");

        // Saturate ordinary workers; a Stop for another device still progresses.
        pool.Start();
        std::promise<void> unblock, safety;
        auto blocked = unblock.get_future().share();
        std::atomic<int> started{0};
        for (int lane = 1; lane <= 3; ++lane)
            Check(pool.Submit({lane, PtzRequestExecutor::Kind::Read, [&] { ++started; blocked.wait(); }, [] {}}), "saturation admission");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (started < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        for (int i = 0; i < 32; ++i)
            Check(pool.Submit({1, PtzRequestExecutor::Kind::Read, [] {}, [&] { ++cancelled; }}), "queue capacity");
        const bool full = !pool.Submit({1, PtzRequestExecutor::Kind::Read, [] {}, [] {}});
        Check(pool.Submit({99, PtzRequestExecutor::Kind::Stop, [&] { safety.set_value(); }, [] {}}), "reserved Stop capacity");
        const bool safetyReady = safety.get_future().wait_for(2s) == std::future_status::ready;
        unblock.set_value(); pool.Stop();
        Check(started == 3 && full && safetyReady, "reserved Stop worker/capacity failed");
        std::cout << "PASS: lane ordering, Stop cancellation/priority, independent progress, bounded admission and shutdown\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
