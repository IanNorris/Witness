#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

// One lane per physical camera (linked views use the same device ID).
// Does not hold GlobalContext or the recording database during camera I/O.
class PtzMovementLease
{
public:
    using Send = std::function<bool()>;
    using Notice = std::function<void(int, bool)>;
    explicit PtzMovementLease(Notice notice = {},
        std::chrono::milliseconds duration = std::chrono::seconds(10),
        std::chrono::milliseconds retry = std::chrono::seconds(1));
    ~PtzMovementLease();
    PtzMovementLease(const PtzMovementLease&) = delete;
    PtzMovementLease& operator=(const PtzMovementLease&) = delete;

    // Arm BEFORE dispatch: a failed acknowledgement can still mean the motor ran.
    bool Move(int deviceId, Send command, Send stop);
    bool Stop(int deviceId, Send stop);
    // No discovery/fallback when the moving transport is already retained.
    // nullopt means no pending movement; false means Stop failed and is retried.
    std::optional<bool> StopExisting(int deviceId);
    // Presets must stop any outstanding manual movement before dispatch.
    bool Position(int deviceId, Send command);
    // Hold from configuration lookup through transport dispatch. Readers on
    // independent cameras remain concurrent; configuration changes are fenced.
    std::shared_lock<std::shared_mutex> AcquireControl();
    // Resolve affected OLD lane identities under the exclusive fence, stop
    // retained transports, then commit. No SQL/query lock may span Stop I/O.
    // A failed Stop leaves the configuration unchanged and retains its retry.
    bool Reconfigure(const std::function<std::vector<int>()>& affected, Send commit);
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
