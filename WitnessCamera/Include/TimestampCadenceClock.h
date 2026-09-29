#pragma once

#include <cstdint>

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
