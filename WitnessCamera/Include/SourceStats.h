#pragma once

struct SourceStats
{
	SourceStats()
	{
		Reset();
	}

	int64_t							LastTimestamp;
	int64_t							FrameCount;
	int64_t							SecondPassFrameCount;
	int64_t							TotalProcessingTime;
	int64_t							ScaleTotalProcessingTime;
	int64_t							MotionDetectionTotalProcessingTime;
	int64_t							SecondPassFilterTotalProcessingTime;
	
	void Reset()
	{
		LastTimestamp = 0;
		FrameCount = 0;
		SecondPassFrameCount = 0;
		TotalProcessingTime = 0;
		ScaleTotalProcessingTime = 0;
		MotionDetectionTotalProcessingTime = 0;
		SecondPassFilterTotalProcessingTime = 0;
	}
};
