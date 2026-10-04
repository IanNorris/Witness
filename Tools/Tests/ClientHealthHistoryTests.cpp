#include "ClientHealthHistory.h"
#include <crow/json.h>
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
    return {999, 123, User, "authenticated-user", "browser-a", std::move(Session),
        Bytes <= 2 ? "{}" : "{\"f\":\"" + std::string(Bytes - 8, 'x') + "\"}"};
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
            auto QuotedOwner = Row(8); QuotedOwner.Username = "quoted\"user\n";
            Save(Store, QuotedOwner);
            auto Export = crow::json::load(Store.ExportJson());
            Check(Export && Export["sessions"].size() == 2 && Export["history"].size() == 2, "invalid JSON export");
            Check(std::string(Export["sessions"][1]["username"].s()) == QuotedOwner.Username, "owner string escaped incorrectly");
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
            const auto ExportStart = std::chrono::steady_clock::now();
            const auto LargeExport = Store.ExportJson();
            std::cout << "Near-limit history export ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - ExportStart).count() << " bytes=" << LargeExport.size() << '\n';
            Check(static_cast<bool>(crow::json::load(LargeExport)), "large history export invalid");
            // Returned snapshots own their strings; callers cannot mutate the
            // immutable store. Readers also race byte-limit pruning and durable
            // writes, so retained references must outlive retired store rows.
            auto OwnedSnapshot = Store.GetSnapshot();
            const auto OriginalPayload = OwnedSnapshot.Samples.back().Json;
            OwnedSnapshot.Samples.back().Json = "caller-owned mutation";
            Check(Store.GetSnapshot().Samples.back().Json == OriginalPayload, "snapshot mutation reached stored row");
            std::atomic<int> ReadersStarted{0}, SnapshotsChecked{0};
            std::atomic<bool> ReadersFailed{false};
            auto ReadConcurrent = [&] {
                ++ReadersStarted;
                try {
                    for (int Round = 0; Round < 16; ++Round) {
                        const auto View = Store.GetSnapshot();
                        size_t TotalBytes = 0;
                        uint64_t PreviousSequence = 0;
                        for (const auto& Sample : View.Samples) {
                            Check(Sample.Sequence > PreviousSequence, "concurrent snapshot order changed");
                            PreviousSequence = Sample.Sequence;
                            Check(static_cast<bool>(crow::json::load(Sample.Json)), "retired snapshot payload invalid");
                            TotalBytes += Sample.Json.size() + Sample.Username.size() + Sample.BrowserID.size() + Sample.SessionID.size();
                        }
                        Check(View.Samples.size() <= History::MaxSamples && TotalBytes <= History::MaxHistoryBytes,
                              "concurrent snapshot exceeds bounds");
                        if (Round % 8 == 0) Check(static_cast<bool>(crow::json::load(Store.ExportJson())), "concurrent export invalid");
                        ++SnapshotsChecked;
                    }
                } catch (...) { ReadersFailed = true; }
            };
            {
                std::jthread ReaderA(ReadConcurrent), ReaderB(ReadConcurrent);
                Await([&] { return ReadersStarted.load() == 2; });
                for (int Index = 0; Index < 32; ++Index) {
                    Time += 15000;
                    Save(Store, Row(100 + Index % 4, "tab-" + std::to_string((Index / 4) % 4), History::MaxPayloadBytes));
                }
            }
            Check(!ReadersFailed && SnapshotsChecked == 32, "concurrent snapshot/pruning regression failed");
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
