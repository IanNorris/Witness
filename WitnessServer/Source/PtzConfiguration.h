#pragma once
#include "SQLite.h"

// Owned snapshots: no SQLite text pointers or query locks cross into discovery
// or transport I/O. Shared by the HTTP route and isolated concurrency tests.
struct PtzConfiguration
{
    bool Enabled = false;
    int Port = 443;
    std::string Host, Username, Password, RtspPath;
};

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
