#include "CapturedClipStorage.h"
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <memory>

void Check(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
int Count(sqlite3* db, const char* sql) {
    sqlite3_stmt* query = nullptr;
    Check(sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK, "Count preparation failed");
    const auto state = sqlite3_step(query); const int result = sqlite3_column_int(query, 0);
    sqlite3_finalize(query); Check(state == SQLITE_ROW, "Count failed"); return result;
}
int main(int argc, char** argv) {
    try {
        Check(argc == 3 && std::filesystem::is_regular_file(argv[1]) && !std::filesystem::exists(argv[2]), "Usage: CapturedClipStorageTests synthetic-media new-directory");
        const auto directory = std::filesystem::absolute(argv[2]); std::filesystem::create_directories(directory);
        const auto path = (directory / "capture.mp4").string(), secondPath = (directory / "capture2.mp4").string();
        std::filesystem::copy_file(argv[1], path); std::filesystem::copy_file(argv[1], secondPath);
        const auto filename = (directory / "clips.sqlite").string();
        sqlite3* raw = nullptr; Check(sqlite3_open(filename.c_str(), &raw) == SQLITE_OK, "Test DB open failed");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
        Check(sqlite3_exec(raw, "PRAGMA foreign_keys=ON; CREATE TABLE Clip(ClipUID INTEGER PRIMARY KEY,Timestamp DATETIME,Camera INT,MotionTimestamp DATETIME,ActiveDuration INT,Duration INT,RecordMode INT,MaxMotion FLOAT,Description TEXT,Save INT,Tags TEXT,DetectionVersion INT,Lighting INT,Reviewed INT,RecordingFinalized INT); CREATE UNIQUE INDEX ClipIndex ON Clip(Timestamp,Camera); INSERT INTO Clip(ClipUID,Timestamp,Camera,Duration,RecordMode,Tags) VALUES(1,1000,8,7,1,'person');", nullptr, nullptr, nullptr) == SQLITE_OK, "Test schema failed");
        EnsureCapturedClipSchema(raw);
        Witness::Camera::HistoricalClipResult media; media.Success = true; media.ActualFrom = 1000;
        media.ActualTo = 1008; media.TimelineFrom = 999.9; media.Coverage = {{1000,1008}};
        const auto first = RegisterCapturedClip(filename, 8, path, media, 1003, 1006);
        Check(first != 1 && RegisterCapturedClip(filename, 8, path, media, 1003, 1006) == first, "Registration was not idempotent");
        const auto second = RegisterCapturedClip(filename, 8, secondPath, media, 1003, 1006);
        Check(second != first && Count(raw, "SELECT COUNT(*) FROM Clip;") == 3, "Repeated time window collided");
        Check(Count(raw, "SELECT COUNT(*) FROM Clip WHERE ClipUID=1 AND Timestamp=1000 AND Duration=7 AND RecordMode=1 AND Tags='person';") == 1,
            "Existing recording was changed");
        Check(Count(raw, "SELECT COUNT(*) FROM Clip WHERE Timestamp=1000 AND Camera=8;") == 1,
            "Timestamp-based recording updates could reach a capture");
        Check(Count(raw, "SELECT COUNT(*) FROM Clip WHERE RecordMode=2 AND Save=1 AND RecordingFinalized=1;") == 2
            && Count(raw, "SELECT COUNT(*) FROM CapturedClipMedia WHERE RequestedFrom=1003 AND RequestedTo=1006;") == 2, "Capture lifecycle/coverage metadata missing");
        bool failed = false; media.Success = false;
        try {RegisterCapturedClip(filename, 8, secondPath, media, 1003, 1006);} catch (...) {failed = true;}
        Check(failed && Count(raw, "SELECT COUNT(*) FROM Clip;") == 3, "Invalid media mutated the library");
        const auto failedPath = (directory / "failed.mp4").string(); std::filesystem::copy_file(argv[1], failedPath);
        Check(sqlite3_exec(raw, "CREATE TRIGGER fail_capture BEFORE INSERT ON CapturedClipMedia BEGIN SELECT RAISE(ABORT,'synthetic failure'); END;", nullptr, nullptr, nullptr) == SQLITE_OK, "Failure trigger setup failed");
        media.Success = true; failed = false;
        try {RegisterCapturedClip(filename, 8, failedPath, media, 1003, 1006);} catch (...) {failed = true;}
        Check(failed && Count(raw, "SELECT COUNT(*) FROM Clip;") == 3 && Count(raw, "SELECT COUNT(*) FROM CapturedClipMedia;") == 2,
            "Metadata failure left a partial clip registration");
        Check(sqlite3_exec(raw, "DROP TRIGGER fail_capture; BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "Busy database setup failed");
        failed = false;
        try {RegisterCapturedClip(filename, 8, failedPath, media, 1003, 1006);} catch (...) {failed = true;}
        Check(sqlite3_exec(raw, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK && failed
            && Count(raw, "SELECT COUNT(*) FROM Clip;") == 3, "Busy database mutated existing clips");
        const auto deletion = "DELETE FROM Clip WHERE ClipUID=" + std::to_string(first);
        Check(sqlite3_exec(raw, deletion.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK
            && Count(raw, "SELECT COUNT(*) FROM CapturedClipMedia;") == 1
            && Count(raw, "SELECT COUNT(*) FROM Clip WHERE ClipUID=1;") == 1, "UID-specific deletion touched unrelated rows");
        std::cout << "PASS capture library: same-time isolation, idempotency, finalized/save flags, metadata, invalid-media guard, UID deletion\n";
        return 0;
    } catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
}
