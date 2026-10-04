#pragma once
#include <chrono>
#include <functional>
#include <memory>

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
    // Presets must stop any outstanding manual movement before dispatch.
    bool Position(int deviceId, Send command);
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
