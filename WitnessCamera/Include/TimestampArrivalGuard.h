#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Witness::Camera
{
// Independent qualification for video-only previews whose declared durations
// are stable but whose raw clock is not. Never derive this evidence from DTS.
struct TimestampArrivalGuard
{
	static constexpr int Capacity = 32;
	int SampleCount = 0;
	int Position = 0;
	int64_t ArrivalTicks = 0;
	int64_t DurationTicks = 0;
	int64_t DeclaredDuration = 0;
	int64_t LastArrival = -1;
	int64_t Arrivals[Capacity] = {};
	int64_t Durations[Capacity] = {};

	void Reset() { *this = {}; }
	static bool Matches(int64_t Value, int64_t Expected)
	{
		return Value > 0 && Expected > 0 &&
			(Value > Expected ? Value - Expected : Expected - Value) <=
			(std::max<int64_t>)(1, Expected / 10);
	}
	static bool SameDeclaration(int64_t Value, int64_t Expected)
	{
		// Allow one tick of rational-duration rounding, not a variable-rate ramp.
		return Value > 0 && Expected > 0 &&
			(Value > Expected ? Value - Expected : Expected - Value) <= 1;
	}
	void Observe(int64_t Arrival, int64_t Duration, bool Valid)
	{
		if (!Valid || Arrival < 0 || Duration <= 0 || Duration > INT32_MAX)
		{
			Reset();
			return;
		}
		if (LastArrival < 0 || !SameDeclaration(Duration, DeclaredDuration) ||
			Arrival < LastArrival || Arrival - LastArrival > Duration * Capacity)
		{
			Reset();
			LastArrival = Arrival;
			DeclaredDuration = Duration;
			return;
		}
		if (SampleCount == Capacity)
		{
			ArrivalTicks -= Arrivals[Position];
			DurationTicks -= Durations[Position];
		}
		else ++SampleCount;
		Arrivals[Position] = Arrival - LastArrival;
		Durations[Position] = DeclaredDuration;
		ArrivalTicks += Arrivals[Position];
		DurationTicks += Durations[Position];
		Position = (Position + 1) % Capacity;
		LastArrival = Arrival;
		// Keep the declaration anchor fixed for this evidence window. Comparing
		// only adjacent declarations would let a gradual VFR ramp look stable.
	}
	bool Ready(int64_t MinimumEvidenceTicks) const
	{
		return SampleCount >= 8 && MinimumEvidenceTicks > 0 &&
			DurationTicks >= MinimumEvidenceTicks && Matches(ArrivalTicks, DurationTicks);
	}
};
}
