#pragma once

#include <cstdint>
#include <cmath>
#include <limits>
#include "CadenceDeliveryEvidence.h"

namespace Witness::Camera
{
// All arguments use the video timebase. Only qualified video-only previews
// select arrival time; audio and provider-profile streams retain source time.
struct TimestampCadenceClock
{
	bool UseArrival = false;
	int64_t ArrivalAnchor = 0;
	int64_t OutputAnchor = 0;
	int64_t WindowArrivalAnchor = 0;
	int64_t LearnedDurationTicks = 0;
	uint64_t RateAdjustments = 0;
	int64_t CandidateDurationTicks = 0;
	unsigned UnstableWindows = 0;

	void Reset() { *this = {}; }
	void StartArrival(int64_t Arrival, int64_t Output)
	{
		Reset();
		UseArrival = true;
		ArrivalAnchor = WindowArrivalAnchor = Arrival;
		OutputAnchor = Output;
	}
	int64_t Target(int64_t SourceTarget, int64_t Arrival) const
	{
		return UseArrival ? OutputAnchor + (Arrival - ArrivalAnchor) : SourceTarget;
	}
	int64_t WindowElapsed(int64_t SourceElapsed, int64_t Arrival) const
	{
		return UseArrival ? Arrival - WindowArrivalAnchor : SourceElapsed;
	}
	void NextWindow(int64_t Arrival) { WindowArrivalAnchor = Arrival; }
	// Only an already-qualified broken source clock can use this estimate.
	// Stable declarations are not necessarily the delivered rate (camera 8:
	// 120s declared / 131.284s delivered). Learn a bounded average, never an
	// individual arrival gap. Two consistent clean windows are required. A
	// fixed mismatch against a demonstrably faulty declaration is not evidence
	// that the underlying source DTS has become trustworthy again.
	// Keep the original 5% source-clock guard unchanged.
	bool LearnArrivalDuration(int64_t Reference, int64_t Declared, uint64_t Samples,
		int64_t MinimumEvidenceTicks, const CadenceDeliveryEvidence& Evidence)
	{
		auto RejectWindow = [&] { CandidateDurationTicks = 0; UnstableWindows = 0; return false; };
		if (!UseArrival || MinimumEvidenceTicks <= 0 || Reference < MinimumEvidenceTicks ||
			Declared <= 0 || Samples < 8 || Samples > static_cast<uint64_t>(INT64_MAX) ||
			Evidence.Packets < Samples - 1 || Evidence.Packets > Samples ||
			Evidence.InvalidDurations || Evidence.DeclarationChanges || Evidence.CorruptPackets ||
			Evidence.ReorderedPackets || !std::isfinite(Evidence.DeclaredMs) || Evidence.DeclaredMs <= 0 ||
			!std::isfinite(Evidence.MaxGapMs) || Evidence.MaxGapMs < 0 || Evidence.MaxGapMs >= 1000 ||
			!std::isfinite(Evidence.LongGapExcessMs) || Evidence.LongGapExcessMs < 0 ||
			!std::isfinite(Evidence.LastDurationMs) || Evidence.LastDurationMs <= 0 || Evidence.LastDurationTicks <= 0)
			return RejectWindow();
		const auto Count = static_cast<int64_t>(Samples);
		const int64_t Duration = Reference / Count + (Reference % Count >= (Count / 2 + Count % 2) ? 1 : 0);
		if (Duration <= 0 || Duration > INT32_MAX) return RejectWindow();
		const double TickMs = Evidence.LastDurationMs / Evidence.LastDurationTicks;
		const double DurationMs = Duration * TickMs;
		if (!std::isfinite(DurationMs) || DurationMs < 1000.0 / 120.0 || DurationMs > 1000.0 ||
			Evidence.LongGapExcessMs > Reference * TickMs * 0.05) return RejectWindow();
		if (CandidateDurationTicks == 0) {
			CandidateDurationTicks = Duration;
			return true; // retain qualified repair for one evidence window, no estimate yet
		}
		const int64_t Difference = Duration > CandidateDurationTicks ?
			Duration - CandidateDurationTicks : CandidateDurationTicks - Duration;
		if (Difference > CandidateDurationTicks / 20) {
			CandidateDurationTicks = Duration;
			return ++UnstableWindows < 2; // bound an unsettled rate; never adopt it
		}
		UnstableWindows = 0;
		const int64_t Estimate = (CandidateDurationTicks + Duration) / 2;
		CandidateDurationTicks = Duration;
		if (Estimate != LearnedDurationTicks) ++RateAdjustments;
		LearnedDurationTicks = Estimate;
		return true;
	}
	static bool CadenceDrifted(int64_t Reference, int64_t Duration)
	{
		// Compare without overflowing integer products for fine timebases.
		return Duration <= 0 || Reference < static_cast<long double>(Duration) * 0.95L ||
			Reference > static_cast<long double>(Duration) * 1.05L;
	}
};
}
