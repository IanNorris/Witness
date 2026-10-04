#pragma once
#include <cstdint>

namespace Witness::Camera {
// Worker-local delivery evidence exported independently of repair machinery.
struct CadenceDeliveryEvidence
{
	uint64_t Packets = 0;
	uint64_t InvalidDurations = 0;
	uint64_t DeclarationChanges = 0;
	uint64_t CorruptPackets = 0;
	uint64_t ReorderedPackets = 0;
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

	void Observe(double ArrivalMs, int64_t DurationTicks, double DurationMs, bool Corrupt, bool Reordered = false)
	{
		++Packets;
		if (Corrupt) ++CorruptPackets;
		if (Reordered) ++ReorderedPackets;
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
}
