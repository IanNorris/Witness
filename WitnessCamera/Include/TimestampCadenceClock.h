#pragma once

#include <cstdint>

namespace Witness::Camera
{
// Worker-local evidence, sampled before any mux timestamp/drop decisions.
// Durations are in milliseconds so exports remain independent of timebase.
struct CadenceDeliveryEvidence
{
	uint64_t Packets = 0;
	uint64_t InvalidDurations = 0;
	uint64_t DeclarationChanges = 0;
	uint64_t CorruptPackets = 0;
	double DeclaredMs = 0;
	double MaxGapMs = 0;
	double LongGapExcessMs = 0;
	double LastArrivalMs = -1;
	double LastDurationMs = 0;
	int64_t LastDurationTicks = 0;
	void NextWindow()
	{
		// Preserve the boundary sample so a pause spanning windows is visible.
		const double Arrival = LastArrivalMs;
		const double Duration = LastDurationMs;
		const int64_t Ticks = LastDurationTicks;
		*this = {};
		LastArrivalMs = Arrival;
		LastDurationMs = Duration;
		LastDurationTicks = Ticks;
	}

	void Observe(double ArrivalMs, int64_t DurationTicks, double DurationMs, bool Corrupt)
	{
		++Packets;
		if (Corrupt) ++CorruptPackets;
		if (LastArrivalMs >= 0 && ArrivalMs >= LastArrivalMs)
		{
			const double Gap = ArrivalMs - LastArrivalMs;
			if (Gap > MaxGapMs) MaxGapMs = Gap;
			// Only count substantial pauses, not ordinary per-frame jitter.
			if (LastDurationMs > 0 && Gap > 250 && Gap > 4 * LastDurationMs)
				LongGapExcessMs += Gap - LastDurationMs;
		}
		LastArrivalMs = ArrivalMs;
		if (DurationTicks <= 0 || DurationTicks > INT32_MAX || DurationMs <= 0)
		{
			++InvalidDurations;
			LastDurationMs = 0;
			LastDurationTicks = 0;
			return;
		}
		if (LastDurationTicks > 0 &&
			(DurationTicks > LastDurationTicks ? DurationTicks - LastDurationTicks :
			 LastDurationTicks - DurationTicks) > 1) ++DeclarationChanges;
		DeclaredMs += DurationMs;
		LastDurationMs = DurationMs;
		LastDurationTicks = DurationTicks;
	}
};

// All arguments use the video timebase. Only qualified video-only previews
// select arrival time; audio and provider-profile streams retain source time.
struct TimestampCadenceClock
{
	bool UseArrival = false;
	int64_t ArrivalAnchor = 0;
	int64_t OutputAnchor = 0;
	int64_t WindowArrivalAnchor = 0;

	void Reset() { *this = {}; }
	void StartArrival(int64_t Arrival, int64_t Output)
	{
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
	static bool CadenceDrifted(int64_t Reference, int64_t Duration)
	{
		// Compare without overflowing integer products for fine timebases.
		return Duration <= 0 || Reference < static_cast<long double>(Duration) * 0.95L ||
			Reference > static_cast<long double>(Duration) * 1.05L;
	}
};
}
