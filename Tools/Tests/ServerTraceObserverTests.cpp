// Actual HTTP observer and prepared-query wrapper; no server/cameras/live DB.
#include "HttpServerDiagnostics.h"
#include "SQLite.h"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
static void Check(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}
static void ChangeSetting(const char* Name, const char* Value) {
#ifdef _WIN32
    Check(_putenv_s(Name, Value) == 0, "setting change failed");
#else
    Check(setenv(Name, Value, 1) == 0, "setting change failed");
#endif
}
static void Http(bool Enabled) {
    using namespace Witness::HttpDiagnostics;
    Check(TracingEnabled() == Enabled, "incorrect HTTP observer setting");
    ChangeSetting("WITNESS_HTTP_TRACE", Enabled ? "0" : "1");
    Check(TracingEnabled() == Enabled, "HTTP observer setting changed after initialization");
    int Loop = 0;
    const auto Sync = BeginRequest("GET", "/fixture/1", &Loop, 0);
    Check((Sync.Id != 0) == Enabled, "unexpected request token coverage");
    CompleteRequest(Sync, 200, 10);
    RecordEventLoopRelease(Sync);
    const auto Async = BeginRequest("POST", "/fixture/2", &Loop, 5);
    RecordEventLoopRelease(Async);
    CompleteRequest(Async, 503, 0);
    std::vector<std::thread> Workers;
    for (int Worker = 0; Worker < 8; ++Worker) Workers.emplace_back([&] {
        for (int Request = 0; Request < 50; ++Request) {
            const auto Token = BeginRequest("GET", "/fixture/3", &Loop, 0);
            CompleteRequest(Token, 200, 0);
            RecordEventLoopRelease(Token);
        }
    });
    for (auto& Worker : Workers) Worker.join();
    const auto Snapshot = GetSnapshot();
    Check(Snapshot.TracingEnabled == Enabled, "incorrect HTTP snapshot coverage");
    Check(Snapshot.InFlight == 0 && Snapshot.ActiveRequests.empty(), "HTTP observer leaked active requests");
    if (Enabled) {
        Check(Snapshot.Started == 402 && Snapshot.Completed == 402 && Snapshot.Errors == 1,
              "HTTP observer lost concurrent or async completion counts");
        Check(Snapshot.ObservedWorkers == 1 && Snapshot.Routes.size() == 2,
              "incorrect worker/normalized route summary");
    } else {
        Check(Snapshot.Started == 0 && Snapshot.Completed == 0 && Snapshot.Errors == 0 &&
              Snapshot.Routes.empty() && Snapshot.RecentSlowRequests.empty() && Snapshot.ObservedWorkers == 0,
              "disabled HTTP observer retained request details");
    }
    const auto Active = BeginRequest("GET", "/fixture/active", &Loop, 0);
    const auto Summary = GetSnapshot(0, false);
    Check(Summary.RecentSlowRequests.empty() && Summary.ActiveRequests.empty() &&
          Summary.InFlight == (Enabled ? 1 : 0), "summary copied active incident details or changed counters");
    Check(GetSnapshot().ActiveRequests.size() == (Enabled ? 1 : 0),
          "scalar HTTP read consumed active incident evidence");
    CompleteRequest(Active, 200, 0);
    RecordEventLoopRelease(Active);
    ClearActiveRequests();
}
static void SQLite(bool Enabled) {
    Check(GetSQLiteDiagnosticsSnapshot().TracingEnabled == Enabled, "incorrect SQLite observer setting");
    ChangeSetting("WITNESS_SQLITE_TRACE", Enabled ? "0" : "1");
    auto DB = std::make_shared<SQLiteDatabase>(":memory:",
        "CREATE TABLE item(id INTEGER PRIMARY KEY,value INTEGER); INSERT INTO item VALUES(1,7);",
        true, [](const std::string&) { throw std::runtime_error("fixture database failure"); });
    DB->CreateQuery("Read", "SELECT value FROM item WHERE id=@id;");
    DB->CreateQuery("Write", "UPDATE item SET value=@value WHERE id=@id;");
    {
        SQLiteDatabaseQueryInstance Write(DB, "Write");
        Write->Bind("@id", 1); Write->Bind("@value", 42);
        Write->Execute({});
    }
    int Value = 0;
    {
        SQLiteDatabaseQueryInstance Read(DB, "Read");
        Read->Bind("@id", 1);
        Read->Execute([&](const SQLiteDatabaseQuery& Row) { Value = Row.GetColumnValueInt(0); return true; });
        if (!Enabled) Check(Read->GetInstanceExecuteTimeUS() == 0, "disabled SQLite observer timed execution");
        // A retained slow scope proves timing records still work in the enabled
        // phase. It is not performance or production contention evidence.
        std::this_thread::sleep_for(120ms);
    }
    Check(Value == 42, "observer setting changed write/bind/read behavior");
    try {
        SQLiteDatabaseQueryInstance Read(DB, "Read");
        Read->Bind("@id", 1);
        Read->Execute([](const SQLiteDatabaseQuery&) -> bool { throw std::runtime_error("callback failure"); });
        Check(false, "callback exception was swallowed");
    } catch (const std::runtime_error& Error) {
        Check(std::string(Error.what()) == "callback failure", "unexpected callback failure");
    }
    {
        std::unique_lock<std::mutex> Probe(DB->GetQuery("Read")->PrepareQueryMutex(), std::try_to_lock);
        Check(Probe.owns_lock(), "query teardown failed to release lock");
    }
    std::future<void> Waiting;
    std::promise<void> Entered;
    {
        SQLiteDatabaseQueryInstance Held(DB, "Read");
        Waiting = std::async(std::launch::async, [&] {
            Entered.set_value();
            SQLiteDatabaseQueryInstance Other(DB, "Read");
            Other->Bind("@id", 1);
            Other->Execute([&](const SQLiteDatabaseQuery& Row) {
                Check(Row.GetColumnValueInt(0) == 42, "concurrent query changed result"); return true;
            });
        });
        Entered.get_future().wait();
        Check(Waiting.wait_for(20ms) == std::future_status::timeout, "query lock was bypassed");
    }
    Check(Waiting.wait_for(2s) == std::future_status::ready, "query lock was not released");
    Waiting.get();
    const auto Snapshot = GetSQLiteDiagnosticsSnapshot();
    const auto Summary = GetSQLiteDiagnosticsSnapshot(false);
    Check(Summary.RecentSlowQueries.empty() && Summary.Queries == Snapshot.Queries &&
          Summary.SlowQueries == Snapshot.SlowQueries,
          "scalar SQLite summary changed counters or copied incident history");
    Check(Snapshot.TracingEnabled == Enabled, "SQLite observer setting changed after initialization");
    if (Enabled) {
        Check(Snapshot.Queries == 5 && Snapshot.SlowQueries >= 1 && !Snapshot.RecentSlowQueries.empty(),
              "enabled SQLite observer did not retain the measured slow scope");
    } else {
        Check(Snapshot.Queries == 0 && Snapshot.SlowQueries == 0 && Snapshot.RecentSlowQueries.empty(),
              "disabled SQLite observer retained query timings");
    }
}
int main(int Argc, char** Argv) {
    if (Argc != 3) return 2;
    try {
        Http(std::string(Argv[1]) == "1");
        SQLite(std::string(Argv[2]) == "1");
        std::cout << "PASS: observer coverage, immutable settings, HTTP accounting and SQL behavior/locking\n";
        return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
