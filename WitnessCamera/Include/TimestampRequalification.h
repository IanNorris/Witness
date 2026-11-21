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
        ResetSpan();
        RecoveredAtMs = -1;
    }
    bool Eligible(int64_t NowMs) const {
        return Attempts < MaxAttempts && EligibleAtMs >= 0 && NowMs >= EligibleAtMs;
    }
    // Strict form: any invalid sample restarts the 10 s streak.
    bool Observe(int64_t NowMs, bool CadenceValid, bool CleanKeyframe) {
        if (!Eligible(NowMs) || !CadenceValid) { StableSinceMs = -1; return false; }
        if (StableSinceMs < 0) StableSinceMs = NowMs;
        return CleanKeyframe && NowMs - StableSinceMs >= 10000;
    }

    // Whole-span form for live previews. Wi-Fi delivery jitter makes a short
    // rolling window occasionally read a few percent off cadence; with the
    // strict form one such sample restarted the streak, so a Tapo preview
    // almost never requalified. Here only a packet fault or a delivery stall
    // (> StallResetMs between packets) restarts it. At a clean keyframe after
    // RequiredSpanMs the accumulated arrival time must match the declared
    // packet durations within 5%, with at most 1 in 5 rolling-window cadence
    // samples off. A failed judgement restarts the span from that keyframe.
    static constexpr int64_t RequiredSpanMs = 10000;
    static constexpr int64_t StallResetMs = 1000;
    int64_t LastObservedMs = -1;
    double DeclaredMs = 0;
    unsigned CadenceSamples = 0;
    unsigned OffCadenceSamples = 0;

    void ResetSpan() {
        StableSinceMs = LastObservedMs = -1;
        DeclaredMs = 0;
        CadenceSamples = OffCadenceSamples = 0;
    }
    bool ObserveSpan(int64_t NowMs, bool PacketValid, bool CadenceSampleValid, double DurationMs, bool CleanKeyframe) {
        if (!Eligible(NowMs) || !PacketValid || !(DurationMs > 0)) { ResetSpan(); return false; }
        if (StableSinceMs >= 0 && NowMs - LastObservedMs > StallResetMs) ResetSpan();
        if (StableSinceMs < 0) StableSinceMs = NowMs;
        else DeclaredMs += DurationMs;
        LastObservedMs = NowMs;
        ++CadenceSamples;
        if (!CadenceSampleValid) ++OffCadenceSamples;
        const int64_t SpanMs = NowMs - StableSinceMs;
        if (!CleanKeyframe || SpanMs < RequiredSpanMs) return false;
        const double Error = static_cast<double>(SpanMs) - DeclaredMs;
        const bool Steady = (Error < 0 ? -Error : Error) <= SpanMs * 0.05 && OffCadenceSamples * 5 <= CadenceSamples;
        if (!Steady) {
            ResetSpan();
            StableSinceMs = LastObservedMs = NowMs;
            CadenceSamples = 1;
        }
        return Steady;
    }

    void Recover() {
        ++Attempts;
        EligibleAtMs = -1;
        ResetSpan();
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
