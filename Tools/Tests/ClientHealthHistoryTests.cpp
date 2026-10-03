#include "ClientHealthHistory.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

using namespace Witness::ClientHealth;
static void Check(bool OK, const char* Message) { if (!OK) throw std::runtime_error(Message); }
template<class Predicate> static void Await(Predicate Ready) {
    const auto End = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!Ready()) {
        Check(std::chrono::steady_clock::now() < End, "history worker timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
static Sample Row(int User = 7, std::string Session = "tab-a", size_t Bytes = 2) {
    return {999, 123, User, "authenticated-user", "browser-a", std::move(Session), std::string(Bytes, 'x')};
}
static void Ready(History& Store) {
    Await([&] { return Store.GetSnapshot().Ready; });
    Check(Store.GetSnapshot().StorageAvailable, "storage initialization failed");
}
static void Save(History& Store, Sample Value) {
    const auto Previous = Store.GetSnapshot().DurableThroughSequence;
    Check(Store.Submit(std::move(Value)) == History::SubmitResult::Accepted, "valid sample rejected");
    Await([&] { return Store.GetSnapshot().DurableThroughSequence > Previous; });
}
int main(int Argc, char** Argv) {
    if (Argc != 2) return 2;
    const std::filesystem::path Path = Argv[1];
    if (std::filesystem::exists(Path)) { std::cerr << "refusing existing fixture\n"; return 2; }
    try {
        std::atomic<int64_t> Time{1800000000000LL};
        auto Clock = [&] { return Time.load(); };
        {
            History Store(Path.string(), Clock); Ready(Store);
            Save(Store, Row());
            auto State = Store.GetSnapshot();
            Check(State.Samples.size() == 1 && State.Samples[0].ReceivedUnixMs == Time.load(), "client controlled receive timestamp");
            Check(State.Samples[0].Sequence != 999, "client controlled sequence");
            Check(Store.Submit(Row()) == History::SubmitResult::RateLimited, "same-session rate limit missing");
            auto Bad = Row(); Bad.BrowserID = "<script>";
            Check(Store.Submit(Bad) == History::SubmitResult::Invalid, "invalid browser identity accepted");
            Bad = Row(); Bad.Json.resize(History::MaxPayloadBytes + 1);
            Check(Store.Submit(Bad) == History::SubmitResult::Invalid, "oversized payload accepted");
            // An identical browser/tab identifier belongs to a different
            // authenticated principal, not to the client claiming the ID.
            Save(Store, Row(8));
            for (int Index = 0; Index < 8; ++Index) Save(Store, Row(9, "burst-" + std::to_string(Index)));
            Check(Store.Submit(Row(9, "rotated-id")) == History::SubmitResult::RateLimited, "rotating IDs bypassed owner rate limit");
            for (int Index = 0; Index < 125; ++Index) { Time += 15000; Save(Store, Row()); }
            State = Store.GetSnapshot();
            size_t Count = 0; for (const auto& Sample : State.Samples) if (Sample.UserUID == 7) ++Count;
            Check(Count == History::MaxSamplesPerSession, "per-session history not bounded");
            for (int Index = 0; Index < 12; ++Index) { Time += 15000; Save(Store, Row(7, "tab-" + std::to_string(Index))); }
            State = Store.GetSnapshot();
            size_t Sessions = 0;
            for (const auto& Sample : State.Samples) if (Sample.UserUID == 7) ++Sessions;
            Check(Sessions == History::MaxSessionsPerUser, "per-user session bound failed");
        }
        {
            History Store(Path.string(), Clock); Ready(Store);
            Check(!Store.GetSnapshot().Samples.empty(), "history did not survive restart");
            Check(Store.GetSnapshot().Samples.back().Username == "authenticated-user", "owner identity lost");
            // New receive time moves forward; no expired rows survive export
            // or reload, and the background worker retires them from disk.
            Time += History::RetentionMs + 1;
            Check(Store.GetSnapshot().Samples.empty(), "expired rows exported");
            Save(Store, Row());
            Check(Store.GetSnapshot().Samples.size() == 1, "expired rows retained after write");
            for (int Index = 0; Index < 160; ++Index) {
                Time += 15000;
                Save(Store, Row(100 + Index % 4, "tab-" + std::to_string((Index / 4) % 4), History::MaxPayloadBytes));
            }
            size_t Bytes = 0; for (const auto& Sample : Store.GetSnapshot().Samples)
                Bytes += Sample.Json.size() + Sample.Username.size() + Sample.BrowserID.size() + Sample.SessionID.size();
            Check(Bytes <= History::MaxHistoryBytes, "global byte bound failed");
            Check(Store.GetSnapshot().Samples.size() > 100, "byte-bound fixture did not populate enough history");
            for (int Index = 0; Index < 80; ++Index) { Time += 15000; Save(Store, Row(200 + Index)); }
        }
        {
            History Store(Path.string(), Clock); Ready(Store);
            std::set<std::pair<int, std::string>> Sessions;
            for (const auto& Sample : Store.GetSnapshot().Samples) Sessions.emplace(Sample.UserUID, Sample.SessionID);
            Check(Sessions.size() == History::MaxSessions, "global session bound not durable");
            Time += History::RetentionMs + 1;
            const auto Previous = Store.GetSnapshot().DurableThroughSequence;
            for (int Index = 0; Index < 2200; ++Index) {
                Time += 15000;
                auto Value = Row(1000 + Index % 20);
                auto Result = Store.Submit(Value);
                if (Result == History::SubmitResult::QueueFull) {
                    Await([&] { return Store.GetSnapshot().Pending < History::MaxPending / 2; });
                    Result = Store.Submit(Value);
                }
                Check(Result == History::SubmitResult::Accepted, "batched sample rejected");
            }
            Await([&] { return Store.GetSnapshot().DurableThroughSequence >= Previous + 2200; });
            Check(Store.GetSnapshot().Samples.size() == History::MaxSamples, "global record limit failed");
        }
        {
            History Store(Path.string(), Clock); Ready(Store);
            Check(Store.GetSnapshot().Samples.size() == History::MaxSamples, "global record bound not durable");
        }
        {
            History Store((Path / "missing" / "health.db").string(), Clock);
            Await([&] { return Store.GetSnapshot().Ready; });
            Check(!Store.GetSnapshot().StorageAvailable && Store.GetSnapshot().StorageFailures == 1, "storage failure hidden");
            Check(Store.Submit(Row()) == History::SubmitResult::NotReady, "accepted telemetry without storage");
        }
        std::filesystem::remove(Path); // This test created the exact fixture above.
        std::cout << "Client health history persistence and bounds passed\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
