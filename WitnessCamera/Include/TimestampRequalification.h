#pragma once
#include <cstdint>

namespace Witness::Camera {
// A rejected preview must earn fresh evidence; never simply relax the guard.
struct TimestampRequalification {
    static constexpr unsigned MaxAttempts = 3;
    unsigned Attempts = 0;
    int64_t EligibleAtMs = -1;
    int64_t StableSinceMs = -1;
    int64_t RecoveredAtMs = -1;

    void Reject(int64_t NowMs) {
        EligibleAtMs = NowMs + (60000LL << Attempts);
        StableSinceMs = RecoveredAtMs = -1;
    }
    bool Eligible(int64_t NowMs) const {
        return Attempts < MaxAttempts && EligibleAtMs >= 0 && NowMs >= EligibleAtMs;
    }
    bool Observe(int64_t NowMs, bool CadenceValid, bool CleanKeyframe) {
        if (!Eligible(NowMs) || !CadenceValid) { StableSinceMs = -1; return false; }
        if (StableSinceMs < 0) StableSinceMs = NowMs;
        return CleanKeyframe && NowMs - StableSinceMs >= 10000;
    }
    void Recover() {
        ++Attempts;
        EligibleAtMs = StableSinceMs = -1;
        RecoveredAtMs = -1;
    }
    void Healthy(int64_t NowMs, bool Normalized) {
        if (!Normalized) { RecoveredAtMs = -1; return; }
        if (Attempts && RecoveredAtMs < 0) RecoveredAtMs = NowMs;
        if (RecoveredAtMs >= 0 && NowMs - RecoveredAtMs >= 600000) {
            Attempts = 0;
            RecoveredAtMs = -1;
        }
    }
};
}
