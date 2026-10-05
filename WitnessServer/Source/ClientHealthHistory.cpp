#include "ClientHealthHistory.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace Witness::ClientHealth {
namespace {
using Key = std::tuple<int, std::string, std::string>;
// Stored rows are immutable. Exports retain a bounded list of references under
// the mutex, then copy/serialize payloads without blocking submit or pruning.
using StoredSample = std::shared_ptr<const Sample>;
Key Identity(const Sample& Value) { return {Value.UserUID, Value.BrowserID, Value.SessionID}; }
size_t Bytes(const Sample& Value) {
    return Value.Json.size() + Value.Username.size() + Value.BrowserID.size() + Value.SessionID.size();
}
bool Identifier(const std::string& Value) {
    return !Value.empty() && Value.size() <= 80 && std::all_of(Value.begin(), Value.end(), [](unsigned char C) {
        return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
               (C >= '0' && C <= '9') || C == '-' || C == '_';
    });
}
bool Valid(const Sample& Value) {
    return Value.UserUID >= 0 && Value.Username.size() <= 256 && Identifier(Value.BrowserID) &&
           Identifier(Value.SessionID) && !Value.Json.empty() && Value.Json.size() <= History::MaxPayloadBytes;
}
struct SqliteFailure : std::runtime_error {
    int Code;
    SqliteFailure(int Result, const char* Message) : std::runtime_error(Message), Code(Result & 0xff) {}
};
void Exec(sqlite3* DB, const char* SQL) {
    const int Result = sqlite3_exec(DB, SQL, nullptr, nullptr, nullptr);
    if (Result != SQLITE_OK) throw SqliteFailure(Result, "telemetry storage operation failed");
}
struct Statement {
    sqlite3_stmt* Value = nullptr;
    Statement(sqlite3* DB, const char* SQL) {
        const int Result = sqlite3_prepare_v2(DB, SQL, -1, &Value, nullptr);
        if (Result != SQLITE_OK) throw SqliteFailure(Result, "telemetry statement failed");
    }
    ~Statement() { sqlite3_finalize(Value); }
    void Text(int Index, const std::string& ValueIn) {
        if (sqlite3_bind_text(Value, Index, ValueIn.data(), static_cast<int>(ValueIn.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("telemetry binding failed");
    }
    void Done() {
        const int Result = sqlite3_step(Value);
        if (Result != SQLITE_DONE) throw SqliteFailure(Result, "telemetry write failed");
        sqlite3_reset(Value);
        sqlite3_clear_bindings(Value);
    }
};
std::string TextColumn(sqlite3_stmt* Row, int Index) {
    const auto* Text = sqlite3_column_text(Row, Index);
    return Text ? std::string(reinterpret_cast<const char*>(Text), sqlite3_column_bytes(Row, Index)) : std::string{};
}
}

struct History::Impl {
    Clock Now;
    std::string Path;
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::deque<StoredSample> Pending, Samples;
    std::map<Key, int64_t> LastAccepted;
    struct UserWindow { int64_t Start = 0; size_t Count = 0; };
    std::map<int, UserWindow> UserRates;
    uint64_t NextSequence = 0, Durable = 0, Rejected = 0, Failures = 0;
    bool Ready = false, Available = false, Stopping = false;
    std::thread Worker;
    Impl(std::string DatabasePath, Clock Time) : Now(std::move(Time)), Path(std::move(DatabasePath)) {
        if (!Now) Now = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(); };
        Worker = std::thread([this] { Run(); });
    }
    ~Impl() {
        { std::lock_guard Lock(Mutex); Stopping = true; }
        Wake.notify_one();
        Worker.join();
    }
    // Called with Mutex held. Return exact row IDs to retire on our private DB.
    std::vector<uint64_t> Prune(int64_t Time) {
        std::vector<uint64_t> Removed;
        size_t TotalBytes = 0;
        std::map<Key, size_t> Counts;
        std::map<int, size_t> UserSessions;
        size_t Sessions = 0, Count = 0;
        // Newest samples win; this bounds sessions as well as total payload.
        for (auto It = Samples.rbegin(); It != Samples.rend();) {
            const Sample& Row = **It;
            const Key ID = Identity(Row);
            const bool NewSession = !Counts.contains(ID);
            const bool Keep = Row.ReceivedUnixMs >= Time - RetentionMs && Row.ReceivedUnixMs <= Time &&
                Count < MaxSamples && TotalBytes + Bytes(Row) <= MaxHistoryBytes &&
                Counts[ID] < MaxSamplesPerSession &&
                (!NewSession || (Sessions < MaxSessions && UserSessions[Row.UserUID] < MaxSessionsPerUser));
            if (!Keep) {
                Removed.push_back(Row.Sequence);
                // operator[] above may have created an empty entry; do not let
                // it count as a retained session on the next older sample.
                if (Counts[ID] == 0) Counts.erase(ID);
                It = std::deque<StoredSample>::reverse_iterator(Samples.erase(std::next(It).base()));
            } else {
                if (NewSession) { ++Sessions; ++UserSessions[Row.UserUID]; }
                ++Counts[ID]; ++Count; TotalBytes += Bytes(Row); ++It;
            }
        }
        return Removed;
    }
    void DeleteRows(sqlite3* DB, const std::vector<uint64_t>& IDs) {
        Statement Delete(DB, "DELETE FROM samples WHERE seq=?");
        for (uint64_t ID : IDs) { sqlite3_bind_int64(Delete.Value, 1, static_cast<sqlite3_int64>(ID)); Delete.Done(); }
    }
    void Load(sqlite3* DB) {
        Statement Read(DB, "SELECT seq,received,user_uid,username,browser,session,payload FROM samples ORDER BY seq DESC LIMIT 2048");
        std::deque<StoredSample> Loaded;
        std::vector<uint64_t> Invalid;
        size_t LoadedBytes = 0;
        int Result;
        while ((Result = sqlite3_step(Read.Value)) == SQLITE_ROW) {
            const auto Sequence = static_cast<uint64_t>(sqlite3_column_int64(Read.Value, 0));
            NextSequence = std::max(NextSequence, Sequence);
            // Reject oversized rows before allocating their text fields.
            if (sqlite3_column_bytes(Read.Value, 6) > static_cast<int>(MaxPayloadBytes) ||
                sqlite3_column_bytes(Read.Value, 3) > 256 || sqlite3_column_bytes(Read.Value, 4) > 80 ||
                sqlite3_column_bytes(Read.Value, 5) > 80) { Invalid.push_back(Sequence); continue; }
            Sample Row{static_cast<uint64_t>(sqlite3_column_int64(Read.Value, 0)), sqlite3_column_int64(Read.Value, 1),
                sqlite3_column_int(Read.Value, 2), TextColumn(Read.Value, 3), TextColumn(Read.Value, 4),
                TextColumn(Read.Value, 5), TextColumn(Read.Value, 6)};
            if (Valid(Row) && LoadedBytes + Bytes(Row) <= MaxHistoryBytes) {
                LoadedBytes += Bytes(Row); Loaded.push_front(std::make_shared<const Sample>(std::move(Row)));
            } else Invalid.push_back(Sequence);
        }
        if (Result != SQLITE_DONE) throw SqliteFailure(Result, "telemetry load failed");
        std::vector<uint64_t> Removed;
        { std::lock_guard Lock(Mutex); Samples = std::move(Loaded); Removed = Prune(Now()); }
        Exec(DB, "BEGIN IMMEDIATE");
        DeleteRows(DB, Invalid);
        DeleteRows(DB, Removed);
        Statement Trim(DB, "DELETE FROM samples WHERE received<? OR seq NOT IN (SELECT seq FROM samples ORDER BY seq DESC LIMIT 2048)");
        sqlite3_bind_int64(Trim.Value, 1, Now() - RetentionMs); Trim.Done();
        Exec(DB, "COMMIT");
    }
    template<class Operation> void RetryBusy(sqlite3* DB, Operation RunOperation) {
        // Only this private storage worker waits. Accepted batch IDs remain
        // unchanged, and durability advances only after a successful commit.
        // Eight retries plus the 200ms SQLite busy timeout bound contention;
        // the wake wait is interruptible so shutdown does not inherit backoff.
        for (int Attempt = 0; ; ++Attempt) {
            try {
                RunOperation();
                std::lock_guard Lock(Mutex); Available = true;
                return;
            } catch (const SqliteFailure& Error) {
                if ((Error.Code != SQLITE_BUSY && Error.Code != SQLITE_LOCKED) || Attempt >= 8) throw;
                sqlite3_exec(DB, "ROLLBACK", nullptr, nullptr, nullptr);
                std::unique_lock Lock(Mutex);
                Available = false; ++Failures;
                if (Wake.wait_for(Lock, std::chrono::milliseconds(100), [this] { return Stopping; })) throw;
            }
        }
    }
    void Run() noexcept {
        sqlite3* DB = nullptr;
        try {
            if (sqlite3_open_v2(Path.c_str(), &DB, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
                throw std::runtime_error("telemetry open failed");
            sqlite3_busy_timeout(DB, 200);
            RetryBusy(DB, [&] {
                Exec(DB, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA max_page_count=8192;"
                    "CREATE TABLE IF NOT EXISTS samples(seq INTEGER PRIMARY KEY,received INTEGER NOT NULL,user_uid INTEGER NOT NULL,"
                    "username TEXT NOT NULL,browser TEXT NOT NULL,session TEXT NOT NULL,payload TEXT NOT NULL)");
                Load(DB);
            });
            { std::lock_guard Lock(Mutex); Ready = Available = true; Durable = NextSequence; }
            while (true) {
                std::deque<StoredSample> Batch;
                std::vector<uint64_t> Removed;
                {
                    std::unique_lock Lock(Mutex);
                    Wake.wait_for(Lock, std::chrono::seconds(15), [this] { return Stopping || !Pending.empty(); });
                    Batch.swap(Pending);
                    for (const auto& Row : Batch) Samples.push_back(Row);
                    Removed = Prune(Now());
                    if (Stopping && Batch.empty() && Removed.empty()) break;
                }
                if (Batch.empty() && Removed.empty()) continue;
                RetryBusy(DB, [&] {
                    Exec(DB, "BEGIN IMMEDIATE");
                    DeleteRows(DB, Removed);
                    Statement Insert(DB, "INSERT INTO samples VALUES(?,?,?,?,?,?,?)");
                    for (const auto& StoredRow : Batch) {
                        const Sample& Row = *StoredRow;
                        if (std::find(Removed.begin(), Removed.end(), Row.Sequence) != Removed.end()) continue;
                        sqlite3_bind_int64(Insert.Value, 1, static_cast<sqlite3_int64>(Row.Sequence));
                        sqlite3_bind_int64(Insert.Value, 2, Row.ReceivedUnixMs);
                        sqlite3_bind_int(Insert.Value, 3, Row.UserUID);
                        Insert.Text(4, Row.Username); Insert.Text(5, Row.BrowserID); Insert.Text(6, Row.SessionID); Insert.Text(7, Row.Json);
                        Insert.Done();
                    }
                    Exec(DB, "COMMIT");
                });
                if (!Batch.empty()) { std::lock_guard Lock(Mutex); Durable = Batch.back()->Sequence; }
            }
        } catch (...) {
            if (DB) sqlite3_exec(DB, "ROLLBACK", nullptr, nullptr, nullptr);
            std::lock_guard Lock(Mutex); Available = false; Ready = true; ++Failures; Pending.clear();
        }
        if (DB) sqlite3_close(DB);
    }
};

History::History(std::string Path, Clock Time) : Data(std::make_unique<Impl>(std::move(Path), std::move(Time))) {}
History::~History() = default;
History::SubmitResult History::Submit(Sample Value) {
    std::lock_guard Lock(Data->Mutex);
    auto Reject = [&](SubmitResult Why) { ++Data->Rejected; return Why; };
    if (!Data->Ready || !Data->Available) return Reject(SubmitResult::NotReady);
    if (!Valid(Value)) return Reject(SubmitResult::Invalid);
    const int64_t Time = Data->Now();
    const Key ID = Identity(Value);
    auto Last = Data->LastAccepted.find(ID);
    if (Last != Data->LastAccepted.end() && Time - Last->second < 1000) return Reject(SubmitResult::RateLimited);
    // Rotating browser/tab IDs must not bypass a principal's aggregate limit.
    if (!Data->UserRates.contains(Value.UserUID) && Data->UserRates.size() >= MaxSessions) {
        auto Oldest = std::min_element(Data->UserRates.begin(), Data->UserRates.end(),
            [](const auto& A, const auto& B) { return A.second.Start < B.second.Start; });
        Data->UserRates.erase(Oldest);
    }
    auto& UserRate = Data->UserRates[Value.UserUID];
    if (Time - UserRate.Start >= 1000) UserRate = {Time, 0};
    if (UserRate.Count >= MaxSessionsPerUser) return Reject(SubmitResult::RateLimited);
    if (Data->Pending.size() >= MaxPending) return Reject(SubmitResult::QueueFull);
    if (Data->LastAccepted.size() >= MaxSessions && Last == Data->LastAccepted.end()) {
        auto Oldest = std::min_element(Data->LastAccepted.begin(), Data->LastAccepted.end(),
            [](const auto& A, const auto& B) { return A.second < B.second; });
        Data->LastAccepted.erase(Oldest);
    }
    Data->LastAccepted[ID] = Time;
    ++UserRate.Count;
    Value.Sequence = ++Data->NextSequence;
    Value.ReceivedUnixMs = Time;
    Data->Pending.push_back(std::make_shared<const Sample>(std::move(Value)));
    Data->Wake.notify_one();
    return SubmitResult::Accepted;
}
Snapshot History::GetSnapshot() const {
    Snapshot Result;
    std::vector<StoredSample> Rows;
    {
        std::lock_guard Lock(Data->Mutex);
        Result = {Data->Ready, Data->Available, Data->Durable, Data->Rejected, Data->Failures, Data->Pending.size(), {}};
        const int64_t Time = Data->Now();
        Rows.reserve(Data->Samples.size());
        for (const auto& Row : Data->Samples)
            if (Row->ReceivedUnixMs >= Time - RetentionMs && Row->ReceivedUnixMs <= Time) Rows.push_back(Row);
    }
    Result.Samples.reserve(Rows.size());
    for (const auto& Row : Rows) Result.Samples.push_back(*Row);
    return Result;
}
std::string History::ExportJson() const {
    const auto State = GetSnapshot(); // No SQLite or recording DB work on this caller.
    auto Quote = [](const std::string& Text) {
        std::string Out = "\"";
        const char* Hex = "0123456789abcdef";
        for (unsigned char C : Text) {
            if (C == '"' || C == '\\') { Out += '\\'; Out += static_cast<char>(C); }
            else if (C < 32) { Out += "\\u00"; Out += Hex[C >> 4]; Out += Hex[C & 15]; }
            else Out += static_cast<char>(C);
        }
        return Out + '"';
    };
    auto RowJson = [&](const Sample& Row) {
        return "{\"sequence\":" + std::to_string(Row.Sequence) + ",\"receivedUnixMs\":" + std::to_string(Row.ReceivedUnixMs) +
            ",\"userUid\":" + std::to_string(Row.UserUID) + ",\"username\":" + Quote(Row.Username) +
            ",\"browserId\":" + Quote(Row.BrowserID) + ",\"sessionId\":" + Quote(Row.SessionID) + ",\"snapshot\":" + Row.Json + "}";
    };
    std::map<Key, const Sample*> Latest;
    std::string Rows;
    for (const auto& Row : State.Samples) { if (!Rows.empty()) Rows += ','; Rows += RowJson(Row); Latest[Identity(Row)] = &Row; }
    std::string Sessions;
    for (const auto& [ID, Row] : Latest) { if (!Sessions.empty()) Sessions += ','; Sessions += RowJson(*Row); }
    return "{\"schemaVersion\":1,\"ready\":" + std::string(State.Ready ? "true" : "false") +
        ",\"storageAvailable\":" + (State.StorageAvailable ? "true" : "false") +
        ",\"durableThroughSequence\":" + std::to_string(State.DurableThroughSequence) +
        ",\"pending\":" + std::to_string(State.Pending) + ",\"rejected\":" + std::to_string(State.Rejected) +
        ",\"storageFailures\":" + std::to_string(State.StorageFailures) +
        ",\"retentionMs\":" + std::to_string(RetentionMs) + ",\"maxHistoryBytes\":" + std::to_string(MaxHistoryBytes) +
        ",\"maxSamples\":" + std::to_string(MaxSamples) + ",\"sessions\":[" + Sessions + "],\"history\":[" + Rows + "]}";
}
}
