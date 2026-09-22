#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Witness::Camera
{

// Evidence that a no-B-frame source has delivered a stable, duration-matched
// decode cadence immediately before its clock moves backwards. This lets the
// live mux distinguish the bounded Tapo sawtooth captured in production from
// an unqualified duplicate or genuinely reordered packet.
struct TimestampRegressionGuard
{
	static constexpr int RequiredStableSamples = 8;

	int StableSamples = 0;
	int64_t StableDurationTicks = 0;

	void Reset()
	{
		StableSamples = 0;
		StableDurationTicks = 0;
	}

	static bool ApproximatelyEqual(int64_t Value, int64_t Expected)
	{
		if (Value <= 0 || Expected <= 0)
			return false;
		const int64_t Difference = Value > Expected ?
			Value - Expected : Expected - Value;
		return Difference <= (std::max<int64_t>)(1, Expected / 10);
	}

	void ObserveMonotonic(int64_t Delta, int64_t PreviousDuration,
		int64_t CurrentDuration, bool PtsEqualsDts)
	{
		if (!PtsEqualsDts || !ApproximatelyEqual(Delta, PreviousDuration) ||
			!ApproximatelyEqual(CurrentDuration, PreviousDuration))
		{
			Reset();
			return;
		}

		StableSamples = (std::min)(StableSamples + 1, 1000);
		if (StableDurationTicks <=
			(std::numeric_limits<int64_t>::max)() - PreviousDuration)
		{
			StableDurationTicks += PreviousDuration;
		}
		else
		{
			StableDurationTicks = (std::numeric_limits<int64_t>::max)();
		}
	}

	bool CanRepair(int64_t Dts, int64_t Pts, int64_t PreviousDts,
		int64_t PreviousDuration, int64_t CurrentDuration,
		bool PayloadSeenRecently, int64_t MinimumEvidenceTicks,
		int64_t MaximumRegressionTicks) const
	{
		if (StableSamples < RequiredStableSamples ||
			StableDurationTicks < MinimumEvidenceTicks ||
			Dts >= PreviousDts || Pts != Dts ||
			PayloadSeenRecently ||
			!ApproximatelyEqual(CurrentDuration, PreviousDuration))
		{
			return false;
		}

		const int64_t Regression = PreviousDts - Dts;
		return Regression > 0 && MaximumRegressionTicks > 0 &&
			Regression <= MaximumRegressionTicks;
	}
};

}
