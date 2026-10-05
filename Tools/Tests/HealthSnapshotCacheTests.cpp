#include "HealthSnapshotCache.h"
#include <atomic>
#include <future>
#include <iostream>

using namespace std::chrono_literals;
static void Check(bool Value, const char* Message)
{
    if (!Value) throw std::runtime_error(Message);
}
template<class Predicate> static void Await(Predicate Ready)
{
    const auto Deadline = HealthSnapshotCache::Clock::now() + 3s;
    while (!Ready())
    {
        if (HealthSnapshotCache::Clock::now() >= Deadline) throw std::runtime_error("test timeout");
        std::this_thread::sleep_for(1ms);
    }
}
int main()
{
    try
    {
        HealthSnapshotCache Cache;
        Check(!Cache.Read().Value, "unstarted cache must not fabricate a sample");
        std::atomic<int> Calls{0};
        Cache.Start([&] { ++Calls; return std::string("{\"value\":1}"); }, 10s);
        Await([&] { return Cache.Read().Value != nullptr; });
        auto Pinned = Cache.Read().Value;
        for (int Index = 0; Index < 1000; ++Index)
            Check(Cache.Read().Value == Pinned, "read recollected or copied entry");
        Check(Calls == 1, "request reads triggered collection");
        auto BeforeStop = HealthSnapshotCache::Clock::now();
        Cache.Stop();
        Check(HealthSnapshotCache::Clock::now() - BeforeStop < 1s, "stop did not interrupt interval");
        Check(!Cache.Read().Value && Pinned->Body == "{\"value\":1}", "stop invalidated owning reader");

        Calls = 0;
        Cache.Start([&] {
            const int Call = ++Calls;
            if (Call == 2) throw std::runtime_error("synthetic observer failure");
            if (Call >= 3)
            {
                std::string Oversized(HealthSnapshotCache::MaximumBytes + 1, ' ');
                Oversized.front() = '{'; Oversized.back() = '}';
                return Oversized;
            }
            return std::string("{\"value\":2}");
        }, 10ms);
        Await([&] { return Cache.Read().FailedCollections >= 2; });
        const auto Failed = Cache.Read();
        Check(Failed.Value && Failed.Value->Body == "{\"value\":2}" && Failed.LastCollectionFailed,
              "failure/oversize replaced last valid sample");
        Check(HealthSnapshotCache::Clock::now() >= Failed.Value->Completed, "invalid age anchor");
        Cache.Stop();

        Calls = 0;
        Cache.Start([&] {
            if (++Calls == 1) throw std::runtime_error("first sample failed");
            return std::string("{\"recovered\":true}");
        }, 10ms);
        Await([&] { return Cache.Read().Value != nullptr; });
        const auto Recovered = Cache.Read();
        Check(Recovered.FailedCollections == 1 && !Recovered.LastCollectionFailed,
              "successful collection did not recover failure status");
        Cache.Stop();

        // A shutdown must wait for active collection, not destroy its captures.
        std::promise<void> Entered, Release;
        auto Gate = Release.get_future().share();
        Cache.Start([&] { Entered.set_value(); Gate.wait(); return std::string("{}"); });
        Entered.get_future().wait();
        auto Stopper = std::async(std::launch::async, [&] { Cache.Stop(); });
        const auto Pending = Stopper.wait_for(30ms);
        Release.set_value();
        Check(Pending == std::future_status::timeout, "stop abandoned active collector");
        Check(Stopper.wait_for(1s) == std::future_status::ready, "collector was not joined");
        Stopper.get();
        Check(!Cache.Read().Value, "stopped cache exposed old epoch");
        bool Invalid = false;
        try { Cache.Start([] { return std::string("{}"); }, 0ms); }
        catch (const std::invalid_argument&) { Invalid = true; }
        Check(Invalid, "zero sampling interval accepted");
        std::cout << "PASS: immutable reads, bounded failures, recovery, cancellable interval and joined collection\n";
        return 0;
    }
    catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
