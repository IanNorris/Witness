#include "WorkerBase.h"
#include <future>
#include <iostream>
#include <stdexcept>
#include "TimerWorker.h"
#include "ClipTiming.h"

using namespace std::chrono_literals;

class IdleWorker final : public WorkerBase
{
public:
    IdleWorker() : WorkerBase(std::make_shared<MessageBus>()) {}
    bool Wait(std::chrono::milliseconds Duration) { return WaitForShutdown(Duration); }
private:
    void WorkerMain() override {}
};

static void Check(bool Value, const char* Message)
{
    if (!Value) throw std::runtime_error(Message);
}

int main()
{
    try
    {
        Check(FinalizedMotionDurationSeconds(120, 100, 0) == 20, "active motion must end with the recording");
        Check(FinalizedMotionDurationSeconds(120, 100, 115) == 15, "quiet tail must not count as motion");
        Check(FinalizedMotionDurationSeconds(120, 100, 125) == 20, "motion end must not exceed clip end");
        Check(FinalizedMotionDurationSeconds(120, 100, 95) == 0, "motion duration must not underflow");
        Check(FinalizedMotionDurationSeconds(95, 100, 0) == 0, "reversed clip boundary must not underflow");
        Check(FinalizedMotionDurationSeconds(100, 100, 0) == 0, "equal boundaries have no duration");
        Check(FinalizedMotionDurationSeconds(120, static_cast<uint64_t>(INT64_MAX), 0) == 0,
              "unset motion must have zero duration");
        Check(FinalizedMotionDurationSeconds(120, 100, uint64_t{1} << 63) == 20,
              "legacy unsigned negative sentinel must not produce a huge duration");

        MessageBusQueue PendingQueue;
        auto FirstMessage = std::make_shared<Message>();
        auto SecondMessage = std::make_shared<Message>();
        auto LateMessage = std::make_shared<Message>();
        PendingQueue.Push(FirstMessage);
        PendingQueue.Push(SecondMessage);
        auto Pending = PendingQueue.TakePending();
        PendingQueue.Push(LateMessage);
        Check(Pending.size() == 2 && Pending[0] == FirstMessage && Pending[1] == SecondMessage,
              "shutdown snapshot must preserve pending FIFO order");
        std::shared_ptr<Message> Remaining;
        Check(PendingQueue.TryPop(Remaining) && Remaining == LateMessage,
              "snapshot must not consume later producer messages");
        Check(PendingQueue.TakePending().empty(), "empty shutdown snapshot must not block");

        IdleWorker Normal;
        auto Started = std::chrono::steady_clock::now();
        Check(!Normal.Wait(50ms), "ordinary idle wait reported cancellation");
        Check(std::chrono::steady_clock::now() - Started >= 45ms, "ordinary idle wait returned early");

        IdleWorker AlreadyStopped;
        AlreadyStopped.RequestShutdown();
        Started = std::chrono::steady_clock::now();
        Check(AlreadyStopped.Wait(30s), "pre-existing stop was missed");
        Check(std::chrono::steady_clock::now() - Started < 500ms, "pre-existing stop still slept");
        AlreadyStopped.RequestShutdown(); // Repeated stop remains safe.

        IdleWorker SharedWait;
        auto First = std::async(std::launch::async, [&] { return SharedWait.Wait(30s); });
        auto Second = std::async(std::launch::async, [&] { return SharedWait.Wait(30s); });
        SharedWait.RequestShutdown();
        Check(First.wait_for(500ms) == std::future_status::ready &&
              Second.wait_for(500ms) == std::future_status::ready, "stop did not wake every waiter");
        Check(First.get() && Second.get(), "shared stop was missed");

        // Exercise both request-before-wait and request-during-wait races.
        for (int Iteration = 0; Iteration != 100; ++Iteration)
        {
            IdleWorker Worker;
            auto Waiter = std::async(std::launch::async, [&] { return Worker.Wait(30s); });
            if (Iteration % 2) std::this_thread::sleep_for(1ms);
            Worker.RequestShutdown();
            Check(Waiter.wait_for(500ms) == std::future_status::ready, "stop did not interrupt idle wait");
            Check(Waiter.get(), "interrupted idle wait reported a timeout");
        }
        // A stop request cannot cancel an in-flight callback. Joining must
        // remain blocked until it returns, before its owning timer is destroyed.
        std::promise<void> CallbackEntered, CallbackReleased, JoinEntered;
        auto Entered = CallbackEntered.get_future();
        auto Release = CallbackReleased.get_future().share();
        auto JoinStarted = JoinEntered.get_future();
        std::atomic<bool> CallbackFinished{false};
        auto Timer = std::make_unique<TimerWorker>(std::make_shared<MessageBus>());
        auto Lifetime = std::make_shared<int>(42);
        std::weak_ptr<int> CallbackLifetime = Lifetime;
        Timer->AddTimer([&, Release, Lifetime] {
            CallbackEntered.set_value();
            Release.wait_for(5s); // Bound cleanup even if an earlier assertion fails.
            Check(*Lifetime == 42, "callback state died before completion");
            CallbackFinished = true;
        }, 3600);
        Lifetime.reset();
        Timer->Start(WorkerBase::Priority::Normal);
        Check(Entered.wait_for(2s) == std::future_status::ready, "timer callback did not start");
        Timer->RequestShutdown();
        auto Joined = std::async(std::launch::async, [&] { JoinEntered.set_value(); Timer->Join(); });
        Check(JoinStarted.wait_for(2s) == std::future_status::ready, "join thread did not start");
        const bool JoinedTooSoon = Joined.wait_for(100ms) == std::future_status::ready;
        const bool StateStillOwned = !CallbackLifetime.expired();
        CallbackReleased.set_value(); // Always release before reporting failure.
        Check(Joined.wait_for(2s) == std::future_status::ready, "released callback did not join");
        Joined.get();
        Check(!JoinedTooSoon, "Join returned while callback was still running");
        Check(StateStillOwned && CallbackFinished, "held callback state was not retained");
        Timer.reset();
        Check(CallbackLifetime.expired(), "callback state survived timer teardown");
        std::cout << "Real held timer callback join test passed\n";
        std::cout << "Worker idle cancellation tests passed\n";
        return 0;
    }
    catch (const std::exception& Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
