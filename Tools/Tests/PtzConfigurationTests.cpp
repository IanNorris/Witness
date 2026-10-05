#include "PtzConfiguration.h"
#include "PtzMovementLease.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;

static void Check(bool Value, const char* Message)
{
    if (!Value) throw std::runtime_error(Message);
}

static std::string ProductionQuery(const std::string& Source, const char* Name)
{
    const std::string Prefix = std::string("std::string ") + Name + " = R\"RAW(";
    const auto Begin = Source.find(Prefix);
    Check(Begin != std::string::npos, "query not found in production source");
    const auto End = Source.find(")RAW\";", Begin + Prefix.size());
    Check(End != std::string::npos, "query terminator not found");
    return Source.substr(Begin + Prefix.size(), End - Begin - Prefix.size());
}

int main(int Argc, char** Argv)
{
    if (Argc != 2) return 2;
    try {
        auto Database = std::make_shared<SQLiteDatabase>(":memory:",
            "CREATE TABLE Camera(CameraUID INTEGER, LinkedCameraId INTEGER, PtzEnabled INTEGER,"
            "PtzApiHost TEXT, PtzApiPort INTEGER, PtzUsername TEXT, PtzPassword TEXT, CameraString TEXT);"
            "INSERT INTO Camera VALUES(10,0,1,'host-a',443,'user-a','secret-a','rtsp-a'),"
            "(11,10,0,NULL,0,NULL,NULL,NULL),(12,0,1,'host-b',80,'user-b','secret-b','rtsp-b');",
            true, [](const std::string&) { throw std::runtime_error("fixture database failure"); });
        const auto Source = ReadFileToString(Argv[1]);
        for (const char* Name : {"GetCameraPtzConfig", "GetLinkedCameraId", "GetPtzLaneId"})
            Database->CreateQuery(Name, ProductionQuery(Source, Name));
        const auto Snapshot = ReadPtzConfiguration(Database, 10);
        Check(Snapshot.Enabled && Snapshot.Port == 443 && Snapshot.Host == "host-a" &&
            Snapshot.Username == "user-a" && Snapshot.Password == "secret-a" && Snapshot.RtspPath == "rtsp-a",
            "incorrect owned config snapshot");
        // Probe the actual query mutex, not a substitute lock in a mock.
        for (const char* Name : {"GetCameraPtzConfig", "GetLinkedCameraId", "GetPtzLaneId"}) {
            std::unique_lock<std::mutex> Lock(Database->GetQuery(Name)->PrepareQueryMutex(), std::try_to_lock);
            Check(Lock.owns_lock(), "returned snapshot retained query lock");
        }
        Check(ReadPtzLinkedCameraId(Database, 11) == 10, "linked physical camera missing");
        Check(ReadPtzLinkedCameraId(Database, 10) == 0, "unlinked camera acquired a parent");
        Check(ReadPtzLinkedCameraId(Database, 999) == 0, "missing camera acquired a parent");
        const auto Empty = ReadPtzConfiguration(Database, 11);
        Check(!Empty.Enabled && Empty.Host.empty() && Empty.Username.empty() && Empty.Password.empty() &&
            Empty.RtspPath.empty(), "NULL values were not safely copied");
        Check(!ReadPtzConfiguration(Database, 999).Enabled, "missing camera enabled PTZ");

        Check(PtzControlAllowed(Database, 10, false), "enabled camera denied PTZ control");
        Check(!PtzControlAllowed(Database, 11, false), "disabled view inherited linked camera control");
        Check(!PtzControlAllowed(Database, 999, false), "missing view allowed PTZ control");
        Check(PtzControlAllowed(Database, 11, true), "disabled view prevented safety Stop");
        // An already discovered/cached transport cannot override a later disable.
        Check(sqlite3_exec(Database->GetDatabase(),
            "UPDATE Camera SET PtzEnabled=0 WHERE CameraUID=10;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "fixture PTZ disable failed");
        Check(!PtzControlAllowed(Database, 10, false), "stale transport capability bypassed disable");
        Check(PtzControlAllowed(Database, 10, true), "disabled moving camera prevented Stop");
        Check(sqlite3_exec(Database->GetDatabase(),
            "UPDATE Camera SET PtzEnabled=1 WHERE CameraUID=10;", nullptr, nullptr, nullptr) == SQLITE_OK,
            "fixture PTZ re-enable failed");
        Check(PtzControlAllowed(Database, 10, false), "re-enabled camera retained disabled policy");

        std::promise<void> Entered, Release;
        auto Ready = Release.get_future().share();
        auto Discovery = std::async(std::launch::async, [&] {
            const int Parent = ReadPtzLinkedCameraId(Database, 11);
            const auto Config = ReadPtzConfiguration(Database, Parent);
            Entered.set_value();
            Ready.wait(); // deliberately stalled transport, with snapshot still alive
            Check(Config.Host == "host-a" && Config.Password == "secret-a", "snapshot text invalidated");
        });
        auto EnteredFuture = Entered.get_future();
        if (EnteredFuture.wait_for(2s) != std::future_status::ready) {
            Release.set_value(); throw std::runtime_error("discovery did not start");
        }
        auto Concurrent = std::async(std::launch::async, [&] {
            Check(ReadPtzConfiguration(Database, 12).Host == "host-b", "other camera query failed");
            Check(ReadPtzLinkedCameraId(Database, 11) == 10, "concurrent linked query failed");
            // Replace the underlying SQLite text while the earlier snapshot is retained.
            Check(sqlite3_exec(Database->GetDatabase(),
                "UPDATE Camera SET PtzApiHost='changed', PtzPassword='changed' WHERE CameraUID=10;",
                nullptr, nullptr, nullptr) == SQLITE_OK, "fixture update failed");
        });
        const bool Independent = Concurrent.wait_for(2s) == std::future_status::ready;
        Release.set_value(); // always release the stalled operation before reporting failure
        Discovery.get(); Concurrent.get();
        Check(Independent, "stalled transport blocked another camera's config query");
        Check(Snapshot.Host == "host-a" && Snapshot.Password == "secret-a", "saved snapshot changed after update");
        Check(ReadPtzConfiguration(Database, 10).Host == "changed", "query did not reset after update");
        Check(ReadPtzLaneId(Database, 10) == 10 && ReadPtzLaneId(Database, 11) == 10,
            "incoming-only parent and child did not share lane");
        Check(ReadPtzLaneId(Database, 12) == 12, "independent camera merged into lane");
        Check(ReadPtzLaneId(Database, 999) == 999, "missing camera acquired another lane");
        Check(sqlite3_exec(Database->GetDatabase(),
            "UPDATE Camera SET LinkedCameraId=11 WHERE CameraUID=10;"
            "INSERT INTO Camera VALUES(13,12,0,NULL,0,NULL,NULL,NULL),"
            "(14,11,0,NULL,0,NULL,NULL,NULL),(15,14,0,NULL,0,NULL,NULL,NULL),"
            "(20,21,0,NULL,0,NULL,NULL,NULL),(21,22,0,NULL,0,NULL,NULL,NULL),"
            "(22,20,0,NULL,0,NULL,NULL,NULL),(30,30,0,NULL,0,NULL,NULL,NULL),"
            "(40,999,0,NULL,0,NULL,NULL,NULL),(41,999,0,NULL,0,NULL,NULL,NULL);",
            nullptr, nullptr, nullptr) == SQLITE_OK, "graph fixture update failed");
        for (int Camera : {10, 11, 14, 15}) Check(ReadPtzLaneId(Database, Camera) == 10, "reciprocal/chain lane mismatch");
        Check(ReadPtzLinkedCameraId(Database, 10) == 11 && ReadPtzLinkedCameraId(Database, 11) == 10,
            "fixture did not reproduce old reciprocal lane disagreement");
        for (int Camera : {12, 13}) Check(ReadPtzLaneId(Database, Camera) == 12, "independent linked pair mismatch");
        for (int Camera : {20, 21, 22}) Check(ReadPtzLaneId(Database, Camera) == 20, "cycle lane mismatch");
        Check(ReadPtzLaneId(Database, 30) == 30, "self-link did not terminate");
        Check(ReadPtzLaneId(Database, 40) == 40 && ReadPtzLaneId(Database, 41) == 41,
            "dangling links merged independent cameras");
        { // Exercise the actual lease with IDs returned by the production SQL.
            std::string Order;
            PtzMovementLease Lease({}, 10s, 10ms);
            Check(Lease.Move(ReadPtzLaneId(Database, 10), [&] { Order += 'L'; return true; },
                [&] { Order += 'S'; return true; }), "first reciprocal move failed");
            Check(Lease.Move(ReadPtzLaneId(Database, 11), [&] { Order += 'R'; return true; },
                [&] { Order += 'T'; return true; }), "replacement reciprocal move failed");
            Check(Lease.Stop(ReadPtzLaneId(Database, 14), [] { return false; }), "linked stop missed moving transport");
            Check(Order == "LSRT", "reciprocal moves did not stop previous transport first");
        }
        { std::unique_lock<std::mutex> Lock(Database->GetQuery("GetPtzLaneId")->PrepareQueryMutex(), std::try_to_lock);
          Check(Lock.owns_lock(), "lane identity retained query lock"); }
        {
            std::atomic<bool> AllowStop{false};
            int Commands = 0;
            PtzMovementLease Lease({}, 10s, 10ms);
            Check(Lease.Move(ReadPtzLaneId(Database, 10), [] { return true; }, [&] { return AllowStop.load(); }), "guarded move failed");
            Check(!Lease.Move(ReadPtzLaneId(Database, 11), [&] { ++Commands; return true; }, [] { return true; }),
                "reciprocal alias bypassed failing Stop");
            Check(!Lease.Position(ReadPtzLaneId(Database, 15), [&] { ++Commands; return true; }),
                "linked preset bypassed failing Stop");
            Check(Commands == 0, "command executed behind failed Stop");
            Check(Lease.Move(ReadPtzLaneId(Database, 13), [&] { ++Commands; return true; }, [] { return true; }),
                "independent linked camera was blocked");
            AllowStop = true;
            Check(Lease.Stop(ReadPtzLaneId(Database, 14), [] { return false; }), "linked Stop did not retain original transport");
        }
        std::cout << "PASS: production SQL, owned snapshots, graph lane identities, linked Stop guards and concurrent discovery\n";
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
