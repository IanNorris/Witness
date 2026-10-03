#pragma once
#include "TimestampArrivalGuard.h"
#include "TimestampCadenceClock.h"

namespace Witness::Camera {
// Only used after a video-only/no-B-frame stream has earned arrival repair.
// A stalled delivery window is not evidence that its source clock is sound.
struct TimestampPauseRecovery {
    static constexpr int64_t PauseMs = 2000;
    static constexpr int64_t MaximumPendingMs = 30000;
    int64_t LastArrivalMs = -1;
    int64_t PendingSinceMs = -1;
    int64_t Declaration = 0;
    int64_t LastGapMs = 0;
    bool Abandoned = false;
    TimestampArrivalGuard Fresh;

    bool Pending() const { return PendingSinceMs >= 0; }
    bool Observe(int64_t NowMs, int64_t ArrivalTicks, int64_t Duration,
                 bool Valid, bool CleanKeyframe, int64_t MinimumEvidenceTicks) {
        if (Declaration == 0 && Valid && Duration > 0 && Duration <= INT32_MAX)
            Declaration = Duration;
        if (LastArrivalMs >= 0 && NowMs - LastArrivalMs >= PauseMs && !Pending() && !Abandoned) {
            PendingSinceMs = NowMs;
            LastGapMs = NowMs - LastArrivalMs;
            Fresh.Reset();
        }
        LastArrivalMs = NowMs;
        if (!Pending()) return false;
        // Do not indefinitely suppress validation on a VFR/invalid stream.
        if (NowMs - PendingSinceMs > MaximumPendingMs) {
            PendingSinceMs = -1;
            Abandoned = true;
            Fresh.Reset();
            return false;
        }
        const bool SameDeclaration = Valid && TimestampArrivalGuard::SameDeclaration(Duration, Declaration);
        Fresh.Observe(ArrivalTicks, Duration, SameDeclaration);
        return SameDeclaration && CleanKeyframe && Fresh.Ready(MinimumEvidenceTicks) &&
            !TimestampCadenceClock::CadenceDrifted(Fresh.ArrivalTicks, Fresh.DurationTicks);
    }
};
}
