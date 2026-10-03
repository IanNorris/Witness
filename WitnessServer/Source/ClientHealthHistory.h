#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Witness::ClientHealth {
// Only validated, credential-free telemetry may enter this store. User identity
// comes from the authenticated server session, never from the submitted JSON.
struct Sample {
    uint64_t Sequence = 0;
    int64_t ReceivedUnixMs = 0;
    int UserUID = -1;
    std::string Username, BrowserID, SessionID, Json;
};
struct Snapshot {
    bool Ready = false;
    bool StorageAvailable = false;
    uint64_t DurableThroughSequence = 0;
    uint64_t Rejected = 0;
    uint64_t StorageFailures = 0;
    size_t Pending = 0;
    std::vector<Sample> Samples;
};
class History {
public:
    static constexpr size_t MaxPayloadBytes = 64 * 1024;
    static constexpr size_t MaxHistoryBytes = 8 * 1024 * 1024;
    static constexpr size_t MaxSamples = 2048;
    static constexpr size_t MaxSessions = 64;
    static constexpr size_t MaxSessionsPerUser = 8;
    static constexpr size_t MaxSamplesPerSession = 120;
    static constexpr size_t MaxPending = 64;
    static constexpr int64_t RetentionMs = 24 * 60 * 60 * 1000LL;
    enum class SubmitResult { Accepted, NotReady, Invalid, RateLimited, QueueFull };
    using Clock = std::function<int64_t()>;
    explicit History(std::string DatabasePath, Clock Now = {});
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;
    SubmitResult Submit(Sample Value);
    Snapshot GetSnapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> Data;
};
}
