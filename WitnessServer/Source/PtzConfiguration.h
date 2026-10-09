#pragma once
#include "SQLite.h"
#include "UrlHelpers.h"

// Owned snapshots: no SQLite text pointers or query locks cross into discovery
// or transport I/O. Shared by the HTTP route and isolated concurrency tests.
struct PtzConfiguration
{
    bool Enabled = false;
    int Port = 443;
    std::string Host, Username, Password, RtspPath;
    bool operator==(const PtzConfiguration& Other) const
    {
        return Enabled == Other.Enabled && Port == Other.Port && Host == Other.Host &&
            Username == Other.Username && Password == Other.Password && RtspPath == Other.RtspPath;
    }
};

// Standard Tapo stream paths use the separate ONVIF transport, never Reolink
// login commands. URL profiles are hints, not general vendor IDs. This helper
// remains the exclusion policy for the Reolink startup/discovery paths.
inline bool PtzKnownUnsupported(const PtzConfiguration& Config)
{
    // Explicit Tapo ONVIF service port also works behind an RTSP proxy where
    // the original /stream1 or /stream2 URL cannot identify the camera brand.
    if (Config.Port == 2020) return true;
    const auto Scheme = Config.RtspPath.find("://");
    const auto Path = Scheme == std::string::npos ? std::string::npos : Config.RtspPath.find('/', Scheme + 3);
    if (Path != std::string::npos) {
        const auto End = Config.RtspPath.find_first_of("?#", Path);
        const auto StreamPath = Config.RtspPath.substr(Path, End - Path);
        if (StreamPath == "/stream1" || StreamPath == "/stream2") return true;
    }
    return false;
}

inline bool PtzDiscoverySupported(const PtzConfiguration& Config)
{
    if (PtzKnownUnsupported(Config)) return false;
    return DetectCameraProfile(Config.RtspPath) == CameraProfile::Reolink || !Config.Host.empty();
}

inline PtzConfiguration ReadPtzConfiguration(const std::shared_ptr<SQLiteDatabase>& Database, int CameraId)
{
    PtzConfiguration Result;
    SQLiteDatabaseQueryInstance Query(Database, "GetCameraPtzConfig");
    Query->Bind("@CameraId", CameraId);
    Query->Execute([&](const SQLiteDatabaseQuery& Row) {
        Result.Enabled = Row.GetColumnValueInt(0) != 0;
        Result.Port = Row.GetColumnValueInt(2);
        auto Copy = [&](int Column) {
            const char* Value = Row.GetColumnValueText(Column);
            return Value ? std::string(Value) : std::string{};
        };
        Result.Host = Copy(1); Result.Username = Copy(3);
        Result.Password = Copy(4); Result.RtspPath = Copy(5);
        return true;
    });
    return Result;
}

// Check the requested view, not a cached/linked transport's capability. Stop
// remains available after PTZ is disabled so an acknowledged move can be ended.
// Authentication/authorization of the requested camera must precede this policy.
inline bool PtzControlAllowed(const std::shared_ptr<SQLiteDatabase>& Database, int CameraId, bool Stop)
{
    return Stop || ReadPtzConfiguration(Database, CameraId).Enabled;
}

inline int ReadPtzLinkedCameraId(const std::shared_ptr<SQLiteDatabase>& Database, int CameraId)
{
    int Result = 0;
    SQLiteDatabaseQueryInstance Query(Database, "GetLinkedCameraId");
    Query->Bind("@CameraId", CameraId);
    Query->Execute([&](const SQLiteDatabaseQuery& Row) {
        Result = Row.GetColumnValueInt(0); return true;
    });
    return Result;
}

// Linked dual-lens views share a safety lane, including reciprocal links and
// incoming-only links. This identity never selects a transport or grants access.
inline int ReadPtzLaneId(const std::shared_ptr<SQLiteDatabase>& Database, int CameraId)
{
    int Result = CameraId;
    SQLiteDatabaseQueryInstance Query(Database, "GetPtzLaneId");
    Query->Bind("@CameraId", CameraId);
    Query->Execute([&](const SQLiteDatabaseQuery& Row) {
        const int Canonical = Row.GetColumnValueInt(0);
        if (Canonical > 0) Result = Canonical;
        return true;
    });
    return Result;
}
