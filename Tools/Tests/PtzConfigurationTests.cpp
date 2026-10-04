#include "PtzConfiguration.h"
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
        for (const char* Name : {"GetCameraPtzConfig", "GetLinkedCameraId"})
            Database->CreateQuery(Name, ProductionQuery(Source, Name));
        const auto Snapshot = ReadPtzConfiguration(Database, 10);
        Check(Snapshot.Enabled && Snapshot.Port == 443 && Snapshot.Host == "host-a" &&
            Snapshot.Username == "user-a" && Snapshot.Password == "secret-a" && Snapshot.RtspPath == "rtsp-a",
            "incorrect owned config snapshot");
        // Probe the actual query mutex, not a substitute lock in a mock.
        for (const char* Name : {"GetCameraPtzConfig", "GetLinkedCameraId"}) {
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
        std::cout << "PASS: production SQL, owned config, NULL/missing/linked cameras and concurrent discovery\n";
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
