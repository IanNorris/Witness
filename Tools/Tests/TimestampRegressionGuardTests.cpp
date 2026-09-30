#include "TimestampRegressionGuard.h"
#include "TimestampCadenceClock.h"
#include "TimestampArrivalGuard.h"

#include <stdexcept>

using Witness::Camera::TimestampRegressionGuard;
using Witness::Camera::TimestampCadenceClock;
using Witness::Camera::TimestampArrivalGuard;

static void Require(bool Condition)
{
	if (!Condition)
		throw std::runtime_error("Timestamp regression guard test failed");
}

int main()
{
	constexpr int64_t FrameDuration = 4500; // 20 fps in a 90 kHz timebase
	TimestampRegressionGuard Guard;
	for (int Index = 0; Index < 8; ++Index)
		Guard.ObserveMonotonic(FrameDuration, FrameDuration, FrameDuration, true);

	// Captured Tapo preview signature: stable no-B-frame cadence followed by a
	// bounded 350 ms DTS sawtooth. The access unit itself remains unique.
	Require(Guard.CanRepair(54000, 54000, 85500, FrameDuration, FrameDuration,
		false, 22500, 90000));

	// Do not generalize the repair to composition-order streams, VFR cadence,
	// duplicate timestamps, large resets, or insufficient evidence.
	Require(!Guard.CanRepair(54000, 58500, 85500, FrameDuration, FrameDuration,
		false, 22500, 90000));
	Require(!Guard.CanRepair(54000, 54000, 85500, FrameDuration, 9000,
		false, 22500, 90000));
	Require(!Guard.CanRepair(85500, 85500, 85500, FrameDuration, FrameDuration,
		false, 22500, 90000));
	Require(!Guard.CanRepair(-90000, -90000, 85500, FrameDuration, FrameDuration,
		false, 22500, 90000));
	Require(!Guard.CanRepair(54000, 54000, 85500, FrameDuration, FrameDuration,
		true, 22500, 90000));

	TimestampRegressionGuard ShortHistory;
	for (int Index = 0; Index < 7; ++Index)
		ShortHistory.ObserveMonotonic(FrameDuration, FrameDuration, FrameDuration, true);
	Require(!ShortHistory.CanRepair(54000, 54000, 85500, FrameDuration,
		FrameDuration, false, 22500, 90000));

	TimestampRegressionGuard VariableRate;
	for (int Index = 0; Index < 8; ++Index)
		VariableRate.ObserveMonotonic(FrameDuration, FrameDuration, FrameDuration, true);
	VariableRate.ObserveMonotonic(9000, FrameDuration, 9000, true);
	Require(!VariableRate.CanRepair(54000, 54000, 85500, FrameDuration,
		FrameDuration, false, 22500, 90000));

	// Replay five minutes of duration-matched delivery with a faulty source
	// clock. A repeated backward step loses one third of source time while
	// arrival cadence stays correct. The old source check rejects at 120s.
	TimestampCadenceClock Clock;
	constexpr int64_t ArrivalOrigin = 900000;
	constexpr int64_t OutputOrigin = 1800000;
	Clock.StartArrival(ArrivalOrigin, OutputOrigin);
	int64_t WindowDuration = 0;
	int64_t SourceElapsed = 0;
	for (int Frame = 1; Frame <= 6000; ++Frame)
	{
		const int64_t Arrival = ArrivalOrigin + Frame * FrameDuration;
		SourceElapsed += FrameDuration;
		if (Frame % 20 == 0) SourceElapsed -= 30000;
		WindowDuration += FrameDuration;
		Require(Clock.Target(OutputOrigin + SourceElapsed, Arrival) ==
			OutputOrigin + Frame * FrameDuration);
		if (WindowDuration >= 120 * 90000)
		{
			Require(TimestampCadenceClock::CadenceDrifted(SourceElapsed, WindowDuration));
			Require(!TimestampCadenceClock::CadenceDrifted(
				Clock.WindowElapsed(SourceElapsed, Arrival), WindowDuration));
			Clock.NextWindow(Arrival);
			WindowDuration = SourceElapsed = 0;
		}
	}
	// Burst jitter is tolerable; sustained VFR/rate mismatch and a long outage
	// still reject normalization rather than silently inventing frame cadence.
	Require(!TimestampCadenceClock::CadenceDrifted(120 * 90000 + 45000, 120 * 90000));
	Require(TimestampCadenceClock::CadenceDrifted(130 * 90000, 120 * 90000));
	Require(TimestampCadenceClock::CadenceDrifted(110 * 90000, 120 * 90000));
	Require(!TimestampCadenceClock::CadenceDrifted(114 * 90000, 120 * 90000));
	Require(!TimestampCadenceClock::CadenceDrifted(126 * 90000, 120 * 90000));
	Require(TimestampCadenceClock::CadenceDrifted(0, 0));
	Clock.Reset();
	Require(!Clock.UseArrival);
	Require(Clock.Target(12345, 99999) == 12345);
	Require(Clock.WindowElapsed(54321, 99999) == 54321);
	Clock.StartArrival(100, 200);
	Require(Clock.Target(-500, 110) == 210);
	Require(Clock.WindowElapsed(-500, 110) == 10);

	// Camera 9 capture: stable 4500-tick declarations, but bimodal source
	// deltas keep resetting the source-derived qualification. Arrival evidence
	// must stay independent of those deltas and span at least one second.
	const int64_t RawDeltas[] = {3510, 4320, 7020, 3060, 3780, 3960, 7200, 3420,
		3600, 3780, 7110, 3510, 3870, 7110, 3150, 3960, 3960, 6930};
	TimestampRegressionGuard BimodalSource;
	for (int Index = 1; Index < 18; ++Index)
		BimodalSource.ObserveMonotonic(RawDeltas[Index - 1], RawDeltas[Index - 1], RawDeltas[Index], true);
	Require(BimodalSource.StableSamples < TimestampRegressionGuard::RequiredStableSamples);
	TimestampArrivalGuard ArrivalEvidence;
	for (int Index = 0; Index <= 24; ++Index)
	{
		ArrivalEvidence.Observe(Index * FrameDuration + (Index % 2 ? 900 : 0), FrameDuration, true);
		if (Index < 20) Require(!ArrivalEvidence.Ready(90000));
	}
	Require(ArrivalEvidence.Ready(90000));
	TimestampRegressionGuard Qualified;
	Qualified.StableSamples = ArrivalEvidence.SampleCount;
	Qualified.StableDurationTicks = ArrivalEvidence.DurationTicks;
	Require(Qualified.CanRepair(54000, 54000, 85500, FrameDuration, FrameDuration, false, 90000, 90000));
	Require(!Qualified.CanRepair(54000, 54000, 85500, FrameDuration, FrameDuration, true, 90000, 90000));
	Require(!Qualified.CanRepair(54000, 58500, 85500, FrameDuration, FrameDuration, false, 90000, 90000));
	Require(!Qualified.CanRepair(-90000, -90000, 85500, FrameDuration, FrameDuration, false, 90000, 90000));
	Require(!Qualified.CanRepair(85500, 85500, 85500, FrameDuration, FrameDuration, false, 90000, 90000));
	for (int Kind = 0; Kind < 3; ++Kind)
	{
		TimestampArrivalGuard InvalidCadence;
		for (int Index = 0; Index < 100; ++Index)
			InvalidCadence.Observe(Kind == 0 ? 0 : Index * 6000,
				Kind == 2 && Index % 2 ? 9000 : FrameDuration, true);
		Require(!InvalidCadence.Ready(90000)); // burst, rate mismatch, VFR declarations
	}
	ArrivalEvidence.Observe(9000000, FrameDuration, true);
	Require(!ArrivalEvidence.Ready(90000)); // network outage discards stale evidence
	ArrivalEvidence.Observe(9004500, FrameDuration, false);
	Require(ArrivalEvidence.SampleCount == 0);
	ArrivalEvidence.Observe(9010000, INT64_MAX, true);
	Require(ArrivalEvidence.SampleCount == 0);
	ArrivalEvidence.Reset();
	for (int Index = 0; Index < 100; ++Index)
		ArrivalEvidence.Observe(Index * FrameDuration, FrameDuration, true);
	Require(ArrivalEvidence.SampleCount == TimestampArrivalGuard::Capacity);
	Require(ArrivalEvidence.Ready(90000));
	for (int Index = 100; Index < 140; ++Index)
		ArrivalEvidence.Observe(99 * FrameDuration + (Index - 99) * 6000, FrameDuration, true);
	Require(!ArrivalEvidence.Ready(90000)); // rolling window, not lifetime average
	TimestampArrivalGuard RampingVfr;
	int64_t VfrArrival = 0;
	int64_t PreviousVfrDuration = FrameDuration;
	for (int Index = 0; Index < 100; ++Index)
	{
		const int64_t Duration = FrameDuration + Index * 90;
		VfrArrival += PreviousVfrDuration;
		RampingVfr.Observe(VfrArrival, Duration, true);
		Require(!RampingVfr.Ready(90000));
		PreviousVfrDuration = Duration;
	}
	TimestampArrivalGuard RationalRounding;
	for (int Index = 0; Index < 100; ++Index)
		RationalRounding.Observe(Index * FrameDuration, FrameDuration + Index % 2, true);
	Require(RationalRounding.Ready(90000));

	return 0;
}
