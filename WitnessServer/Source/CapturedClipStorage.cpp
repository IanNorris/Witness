#include "CapturedClipStorage.h"
#include <sqlite3.h>
#include <crow/json.h>
#include <filesystem>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
struct Statement {sqlite3_stmt* Value = nullptr; ~Statement() {sqlite3_finalize(Value);}};
void Prepare(sqlite3* db, Statement& statement, const char* sql) {
    Check(sqlite3_prepare_v2(db, sql, -1, &statement.Value, nullptr) == SQLITE_OK, "Capture clip query preparation failed");
}
std::string Ranges(const std::vector<Witness::Camera::HistoricalClipRange>& ranges) {
    std::vector<crow::json::wvalue> items;
    for (const auto& range : ranges) {crow::json::wvalue item; item["from"] = range.From; item["to"] = range.To; items.push_back(std::move(item));}
    crow::json::wvalue value(std::move(items)); return value.dump();
}
}
void EnsureCapturedClipSchema(sqlite3* database) {
    Check(sqlite3_exec(database, "CREATE TABLE IF NOT EXISTS CapturedClipMedia(ClipUID INTEGER PRIMARY KEY, FilePath TEXT NOT NULL UNIQUE, ThumbnailPath TEXT NOT NULL, RequestedFrom REAL NOT NULL, RequestedTo REAL NOT NULL, ActualFrom REAL NOT NULL, ActualTo REAL NOT NULL, TimelineFrom REAL NOT NULL, Coverage TEXT NOT NULL, Gaps TEXT NOT NULL, FOREIGN KEY(ClipUID) REFERENCES Clip(ClipUID) ON DELETE CASCADE);", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not initialize captured clip metadata");
}
int64_t RegisterCapturedClip(const std::string& databaseFilename, int camera, const std::string& path,
    const Witness::Camera::HistoricalClipResult& media, double requestedFrom, double requestedTo) {
    Check(camera > 0 && media.Success && std::isfinite(media.ActualFrom) && std::isfinite(media.ActualTo)
        && std::isfinite(media.TimelineFrom) && media.TimelineFrom >= 0 && media.ActualTo > media.TimelineFrom
        && std::isfinite(requestedFrom) && std::isfinite(requestedTo) && requestedFrom >= 0 && requestedTo > requestedFrom
        && media.ActualFrom >= 0 && media.ActualTo > media.ActualFrom && std::filesystem::is_regular_file(path), "Invalid captured clip media");
    sqlite3* raw = nullptr;
    const auto status = sqlite3_open_v2(databaseFilename.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    Check(status == SQLITE_OK, "Could not open capture clip database connection");
    sqlite3_busy_timeout(raw, 1000);
    Check(sqlite3_exec(raw, "PRAGMA foreign_keys=ON; BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "Capture clip database is busy");
    try {
        Statement existing;
        Prepare(raw, existing, "SELECT ClipUID FROM CapturedClipMedia WHERE FilePath=?;");
        Check(sqlite3_bind_text(existing.Value, 1, path.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK, "Capture media binding failed");
        const auto found = sqlite3_step(existing.Value);
        if (found == SQLITE_ROW) {
            const int64_t id = sqlite3_column_int64(existing.Value, 0);
            sqlite3_reset(existing.Value);
            Check(sqlite3_exec(raw, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK, "Capture clip commit failed"); return id;
        }
        Check(found == SQLITE_DONE, "Capture media lookup failed");
        // Normal recording timestamps are whole seconds. Captures use a tiny
        // fractional discriminator and UID-addressed media, preserving the
        // existing unique index and avoiding timestamp-based recording updates.
        const double second = std::floor(media.ActualFrom);
        double timestamp = second + 0.001;
        Statement available;
        Prepare(raw, available, "SELECT 1 FROM Clip WHERE Camera=? AND Timestamp=?;");
        for (int attempt = 0;; ++attempt) {
            Check(attempt < 10000, "Too many captures at this camera timestamp");
            Check(sqlite3_bind_int(available.Value, 1, camera) == SQLITE_OK
                && sqlite3_bind_double(available.Value, 2, timestamp) == SQLITE_OK, "Capture timestamp binding failed");
            const int row = sqlite3_step(available.Value); sqlite3_reset(available.Value);
            Check(row == SQLITE_ROW || row == SQLITE_DONE, "Capture timestamp lookup failed");
            if (row == SQLITE_DONE) break;
            timestamp = second + 0.001 + (attempt + 1) * 0.00001;
        }
        Statement insert;
        Prepare(raw, insert, "INSERT INTO Clip(Timestamp,Camera,MotionTimestamp,ActiveDuration,Duration,RecordMode,MaxMotion,Description,Save,Tags,DetectionVersion,Lighting,Reviewed,RecordingFinalized) VALUES(?,?,0,0,?,2,0,'Retroactive manual capture',1,'',0,0,0,1);");
        Check(sqlite3_bind_double(insert.Value, 1, timestamp) == SQLITE_OK
            && sqlite3_bind_int(insert.Value, 2, camera) == SQLITE_OK
            && sqlite3_bind_int64(insert.Value, 3, static_cast<int64_t>(std::ceil(media.ActualTo - media.TimelineFrom))) == SQLITE_OK
            && sqlite3_step(insert.Value) == SQLITE_DONE, "Could not register captured clip");
        const auto uid = sqlite3_last_insert_rowid(raw);
        const auto coverage = Ranges(media.Coverage), gaps = Ranges(media.Gaps);
        auto thumbnail = std::filesystem::path(path); thumbnail.replace_extension(".jpg");
        Statement metadata;
        Prepare(raw, metadata, "INSERT INTO CapturedClipMedia VALUES(?,?,?,?,?,?,?,?,?,?);");
        Check(sqlite3_bind_int64(metadata.Value, 1, uid) == SQLITE_OK
            && sqlite3_bind_text(metadata.Value, 2, path.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_bind_text(metadata.Value, 3, thumbnail.string().c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_bind_double(metadata.Value, 4, requestedFrom) == SQLITE_OK
            && sqlite3_bind_double(metadata.Value, 5, requestedTo) == SQLITE_OK
            && sqlite3_bind_double(metadata.Value, 6, media.ActualFrom) == SQLITE_OK
            && sqlite3_bind_double(metadata.Value, 7, media.ActualTo) == SQLITE_OK
            && sqlite3_bind_double(metadata.Value, 8, media.TimelineFrom) == SQLITE_OK
            && sqlite3_bind_text(metadata.Value, 9, coverage.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_bind_text(metadata.Value, 10, gaps.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_step(metadata.Value) == SQLITE_DONE, "Could not register capture media metadata");
        Check(sqlite3_exec(raw, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK, "Capture clip commit failed"); return uid;
    } catch (...) {sqlite3_exec(raw, "ROLLBACK;", nullptr, nullptr, nullptr); throw;}
}
