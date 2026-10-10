#include "RecentCaptureJournal.h"
#include <crow/json.h>
#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>
#include <cmath>
#include <charconv>

namespace {
void Require(bool ok, const char* error) {if (!ok) throw std::runtime_error(error);}
using Snapshot = RecentCaptureJobs::Snapshot;
struct Statement {
    sqlite3_stmt* Value = nullptr;
    ~Statement() {sqlite3_finalize(Value);}
};
std::string Encode(const Snapshot& value) {
    crow::json::wvalue item;
    item["id"] = std::to_string(value.Id); item["owner"] = value.Capture.Owner; item["camera"] = value.Capture.Camera;
    item["from"] = value.Capture.From; item["to"] = value.Capture.To; item["destination"] = value.Capture.Destination;
    item["wait"] = value.Capture.FinalizationWaitSeconds; item["state"] = static_cast<int>(value.Status);
    item["success"] = value.Result.Success; item["error"] = value.Result.Error;
    item["actualFrom"] = value.Result.ActualFrom; item["actualTo"] = value.Result.ActualTo;
    item["timelineFrom"] = value.Result.TimelineFrom;
    item["videoPackets"] = value.Result.VideoPackets; item["audioPackets"] = value.Result.AudioPackets;
    std::vector<crow::json::wvalue> coverage, gaps;
    for (const auto& range : value.Result.Coverage) {crow::json::wvalue r; r["from"] = range.From; r["to"] = range.To; coverage.push_back(std::move(r));}
    for (const auto& range : value.Result.Gaps) {crow::json::wvalue r; r["from"] = range.From; r["to"] = range.To; gaps.push_back(std::move(r));}
    item["coverage"] = std::move(coverage); item["gaps"] = std::move(gaps); return item.dump();
}
Snapshot Decode(const std::string& text) {
    auto value = crow::json::load(text); Require(static_cast<bool>(value), "Invalid capture journal JSON");
    Snapshot result;
    const auto idText = std::string(value["id"].s());
    const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), result.Id);
    Require(!idText.empty() && parsed.ec == std::errc{} && parsed.ptr == idText.data() + idText.size(), "Invalid capture journal ID");
    result.Capture.Owner = static_cast<int>(value["owner"].i()); result.Capture.Camera = static_cast<int>(value["camera"].i());
    result.Capture.From = value["from"].d(); result.Capture.To = value["to"].d();
    result.Capture.Destination = std::string(value["destination"].s()); result.Capture.FinalizationWaitSeconds = static_cast<int>(value["wait"].i());
    const int state = static_cast<int>(value["state"].i());
    Require(state >= 0 && state <= static_cast<int>(RecentCaptureJobs::State::Cancelled), "Invalid capture journal state");
    result.Status = static_cast<RecentCaptureJobs::State>(state);
    result.Result.Success = value["success"].b(); result.Result.Error = std::string(value["error"].s());
    result.Result.ActualFrom = value["actualFrom"].d(); result.Result.ActualTo = value["actualTo"].d();
    result.Result.TimelineFrom = value["timelineFrom"].d();
    result.Result.VideoPackets = value["videoPackets"].u(); result.Result.AudioPackets = value["audioPackets"].u();
    for (const auto& range : value["coverage"]) result.Result.Coverage.push_back({range["from"].d(), range["to"].d()});
    for (const auto& range : value["gaps"]) result.Result.Gaps.push_back({range["from"].d(), range["to"].d()});
    Require(result.Capture.Owner > 0 && result.Capture.Camera > 0 && std::isfinite(result.Capture.From)
        && std::isfinite(result.Capture.To) && result.Capture.From >= 0
        && (result.Capture.To == 0 || result.Capture.To > result.Capture.From)
        && result.Capture.FinalizationWaitSeconds >= 0 && result.Capture.FinalizationWaitSeconds <= 960
        && result.Result.Coverage.size() <= 1024 && result.Result.Gaps.size() <= 1024, "Invalid capture journal metadata");
    return result;
}
}
struct RecentCaptureJournal::Impl {
    sqlite3* Database = nullptr;
    std::filesystem::path Directory;
    ~Impl() {sqlite3_close(Database);}
};
RecentCaptureJournal::RecentCaptureJournal(const std::string& filename) : Data(std::make_unique<Impl>()) {
    Data->Directory = std::filesystem::absolute(std::filesystem::path(filename).parent_path()).lexically_normal();
    std::filesystem::create_directories(Data->Directory);
    Require(sqlite3_open_v2(filename.c_str(), &Data->Database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) == SQLITE_OK,
        "Could not open capture journal");
    sqlite3_busy_timeout(Data->Database, 1000);
    Require(sqlite3_exec(Data->Database, "PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS CaptureJob(JobUID INTEGER PRIMARY KEY, Snapshot TEXT NOT NULL);", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Could not initialize capture journal");
}
RecentCaptureJournal::~RecentCaptureJournal() = default;
std::vector<Snapshot> RecentCaptureJournal::Load() {
    Statement statement;
    Require(sqlite3_prepare_v2(Data->Database, "SELECT JobUID, Snapshot FROM CaptureJob ORDER BY JobUID;", -1, &statement.Value, nullptr) == SQLITE_OK, "Could not read capture journal");
    std::vector<Snapshot> values;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement.Value)) == SQLITE_ROW) {
        Require(values.size() < 64 && sqlite3_column_bytes(statement.Value, 1) <= 262144, "Capture journal exceeds size limit");
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.Value, 1));
        Require(text != nullptr, "Missing capture journal snapshot");
        auto value = Decode(text);
        Require(value.Id == static_cast<uint64_t>(sqlite3_column_int64(statement.Value, 0)), "Capture journal ID mismatch");
        Require(std::filesystem::absolute(std::filesystem::path(value.Capture.Destination).parent_path()).lexically_normal() == Data->Directory,
            "Capture journal destination is outside its storage directory");
        values.push_back(std::move(value));
    }
    Require(status == SQLITE_DONE, "Capture journal read failed"); return values;
}
void RecentCaptureJournal::Save(const std::vector<Snapshot>& values) {
    Require(values.size() <= 64, "Capture journal capacity exceeded");
    Require(sqlite3_exec(Data->Database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK, "Capture journal is unavailable");
    try {
        // This is a private journal, not Witness's production clip database.
        // Replacement and all inserts are atomic; a failed save preserves the prior checkpoint.
        Require(sqlite3_exec(Data->Database, "DELETE FROM CaptureJob;", nullptr, nullptr, nullptr) == SQLITE_OK, "Capture journal replacement failed");
        Statement statement;
        Require(sqlite3_prepare_v2(Data->Database, "INSERT INTO CaptureJob(JobUID, Snapshot) VALUES(?,?);", -1, &statement.Value, nullptr) == SQLITE_OK, "Capture journal insert preparation failed");
        for (const auto& value : values) {
            const auto encoded = Encode(value); Require(encoded.size() <= 262144 && value.Id <= INT64_MAX, "Capture journal snapshot exceeds limit");
            Require(sqlite3_bind_int64(statement.Value, 1, static_cast<int64_t>(value.Id)) == SQLITE_OK
                && sqlite3_bind_text(statement.Value, 2, encoded.c_str(), static_cast<int>(encoded.size()), SQLITE_TRANSIENT) == SQLITE_OK
                && sqlite3_step(statement.Value) == SQLITE_DONE, "Capture journal insert failed");
            sqlite3_reset(statement.Value); sqlite3_clear_bindings(statement.Value);
        }
        Require(sqlite3_exec(Data->Database, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK, "Capture journal commit failed");
    } catch (...) {sqlite3_exec(Data->Database, "ROLLBACK;", nullptr, nullptr, nullptr); throw;}
}
