#include "TimestampRegressionGuard.h"

#include <stdexcept>

using Witness::Camera::TimestampRegressionGuard;

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

	return 0;
}
